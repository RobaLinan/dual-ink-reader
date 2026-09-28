// 双屏电子书验证程序（ESP32-S3 N16R8，见 DESIGN.md）
//
// 职责分层（本文件只做组装 + 刷新节奏，逻辑都在 src/）：
//   EinkPanel  单屏驱动（UC8179，三档速度，DTM1 写真实上一帧）
//   DualEink   双屏重叠刷新 present()：L 灌帧→DRF→趁 L busy 给 R 灌帧→DRF→等两屏
//   Storage    SDMMC 1-bit @20MHz 挂载（正式引脚 38/39/40）
//   Book       统一书接口（TxtBook=TXT / EpubBook=EPUB，P4 起书库多书切换）
//   PageStats  刷新耗时统计（min/avg/max + 超时，每 100 页汇总）
//   DemoPage   兜底页码页（SD/书不可用时）
//
// 节奏：每 5 次快刷插 1 次全刷（DESIGN §5.9 = Diptyx partialUpdatesRemaining=5）；
// 降级链：右屏超时→单屏刷，SD/书失败→页码 demo，都不挡测试。
//
// 页面方向：竖版 480×648（短边为横边），PAGE_ROTATE=90 旋转进 648×480 显存
// （页顶朝屏右边 = 阅读时设备逆时针转 90°；宏见 src/EinkPanel.h）
//
// P4 书库/进度：开机扫 /books/*.txt + *.epub 成书库（串口 b 切下一本）；
// 进度（路径+位置+页号+章号）存 NVS，每对页翻完/切书/深睡时写，开机按存档恢复。

#include <ESP32epdx.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <sys/time.h>   // gettimeofday：入睡/醒后时间戳，算深睡时长（自醒取证）
#include "src/EinkPanel.h"
#include "src/DualEink.h"
#include "src/TxtBook.h"
#include "src/EpubBook.h"
#include "src/CjkFont.h"
#include "src/PageStats.h"
#include "src/DemoPage.h"
#include "src/Storage.h"
#include <SD_MMC.h>

// === 引脚分配（DESIGN §1.1）===
#define EINK_SCK   12
#define EINK_MOSI  11
// 左屏
#define PIN_L_CS   14
#define PIN_L_DC   15
#define PIN_L_RST  16
#define PIN_L_BUSY 17
// 右屏（BUSY 48 与 DevKitC-1 的 RGB LED 同脚：LED DIN 高阻，无电气冲突，
// 只是刷新时 LED 可能跟着 BUSY 闪）
#define PIN_R_CS   18
#define PIN_R_DC   21
#define PIN_R_RST  47
#define PIN_R_BUSY 48
// 右屏按"物理旋转 180° 安装"处理（装机形态）。面包板平摆时右屏图像会上下
// 颠倒 —— 想同向：右屏实物转 180° 摆，或把这里改 0
#define R_MIRRORED 1

// 霍尔（WIRING §1/§3.5，台架验证 2026-09-24）：DRV5032 推挽输出，磁铁靠近=LOW
#define PIN_HALL        4
#define HALL_CLOSED_LEVEL 0   // 合盖电平；反相模块改 1（唤醒模式随之下方自动反）
// 深睡时 hold 的 SPI 输出脚：深睡数字脚全高阻，CS/SCK 浮空噪声可能给屏发垃圾
// 命令（VCI 常供 = 屏醒着）。hold 住 = 锁存最近事务后的空闲电平（CS 高、SCK 低）。
// BUSY(17/48) 是屏的输出，高阻无害，不参与 hold
static const int SLEEP_HOLD_PINS[] = {EINK_SCK, EINK_MOSI,
                                      PIN_L_CS, PIN_L_DC, PIN_L_RST,
                                      PIN_R_CS, PIN_R_DC, PIN_R_RST};

// 深睡跨醒保留（RTC 内存）：屏上那一对页的位置/页号/章号（EPUB）
RTC_DATA_ATTR long s_sleepFilePos = -1;
RTC_DATA_ATTR uint32_t s_sleepPage = 0;
RTC_DATA_ATTR int16_t s_sleepChapter = -1;   // ≥0 = EPUB 章号；-1 = TXT/无
RTC_DATA_ATTR time_t s_sleepAt = 0;          // 入睡 wall-clock：醒后打印实际睡了
                                             // 多久（区分"秒级自醒=入睡毛刺"和
                                             // "分钟级=盖子真被打开过"）

static const uint32_t FULL_EVERY = 5;        // 每 5 次快刷插 1 次全刷
static const uint32_t PAGE_INTERVAL_MS = 5000;
static const uint32_t FAST_TARGET_MS = 1800; // 验收线：整页 ≤1.8s
static const uint32_t SAVE_EVERY_PAIRS = 10; // 进度落盘频率（对页数）

EinkPanel left(PIN_L_CS, PIN_L_DC, PIN_L_RST, PIN_L_BUSY, EINK_SCK, EINK_MOSI);
EinkPanel right(PIN_R_CS, PIN_R_DC, PIN_R_RST, PIN_R_BUSY,
                EINK_SCK, EINK_MOSI, R_MIRRORED != 0);
DualEink dual(left, right);
TxtBook txt;
EpubBook epub;
PageStats stats;
Preferences prefs;              // 字体/字号/当前书/进度存 NVS（断电不丢）

// === P4 书库：/books 下的 *.txt / *.epub（根目录旧布局兜底）===
struct LibEntry { char path[64]; bool epub; };   // 64 = "/books/" 前缀 + 长文件名不截断
static LibEntry s_lib[16];
static int s_nlib = 0;
static int s_libIdx = -1;

static bool hasExt(const char* name, const char* ext) {
  size_t nl = strlen(name), el = strlen(ext);
  if (nl <= el) return false;
  const char* s = name + nl - el;
  for (size_t i = 0; i < el; i++)
    if (tolower((unsigned char)s[i]) != tolower((unsigned char)ext[i])) return false;
  return true;
}

static void addLib(const char* path, bool epubFlag) {
  if (s_nlib >= (int)(sizeof(s_lib) / sizeof(s_lib[0]))) return;
  snprintf(s_lib[s_nlib].path, sizeof(s_lib[s_nlib].path), "%s", path);
  s_lib[s_nlib].epub = epubFlag;
  s_nlib++;
}

// 扫 /books（+ 根目录旧布局两个测试文件）。SD 不可用则空库
static void scanLibrary() {
  s_nlib = 0;
  if (!Storage::begin()) return;
  static const char kBooksDir[] = "/books";
  File dir = SD_MMC.open(kBooksDir);
  if (dir) {
    for (File f = dir.openNextFile(); f && s_nlib < 16; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      // 必须用 path()：core 2.0.17 的 name() 走 pathToFileName() 只回 basename，
      // 存裸名会丢 /books/ 前缀 → open() 因"不以 / 开头"被 VFSImpl::open 拒掉
      // （vfs_api.cpp "does not start with /"），于是每次开机都"列得出书却
      // 打不开"、永远退 demo 页。path() 才是全路径
      const char* n = f.path();
      char built[64];
      if (!n || n[0] != '/') {        // 兜底：极少数 core 不给全路径就自己拼
        snprintf(built, sizeof(built), "%s/%s", kBooksDir, f.name() ? f.name() : "");
        n = built;
      }
      if (hasExt(n, ".txt")) addLib(n, false);
      else if (hasExt(n, ".epub")) addLib(n, true);
    }
    dir.close();
  }
  if (SD_MMC.open("/test_cn.txt")) addLib("/test_cn.txt", false);  // 旧布局兜底
  if (SD_MMC.open("/test.txt")) addLib("/test.txt", false);
  for (int i = 0; i < s_nlib; i++)
    Serial.printf("[lib] %d: %s%s\n", i, s_lib[i].path, s_lib[i].epub ? " (epub)" : "");
}

Book* book = nullptr;      // 当前书（nullptr = 无书，demo 页）
bool bookIsEpub = false;

static uint8_t g_size = TXT_SIZE_PX;   // 当前字号档（16/24/32）
static int g_fam = 0;                  // 当前家族（0=classic）

static void printFontState() {
  Serial.printf("[font] family=%s size=%u book=%s (serial: f=font s=size b=next book h=status)\n",
                CJK.familyName(g_fam), g_size,
                s_libIdx >= 0 ? s_lib[s_libIdx].path : "-");
}

// 进度落盘：路径 + 对页位置/页号（+EPUB 章号）。调用点：翻页周期、切书、深睡
static void saveProgress() {
  if (!book || s_libIdx < 0) return;
  prefs.putString("book", s_lib[s_libIdx].path);
  prefs.putLong("pos", book->pairFilePos());
  prefs.putUInt("page", book->pairPageNo());
  prefs.putShort("ch", bookIsEpub ? (int16_t)epub.pairChapter() : (int16_t)-1);
}

// 打开书库第 idx 本。true = 打开成功（旧进度由调用方按需恢复）
static bool openBook(int idx) {
  if (idx < 0 || idx >= s_nlib) return false;
  if (book) book->close();      // 旧书句柄先关（进度已存盘；SD 重挂前必须无旧句柄）
  bookIsEpub = s_lib[idx].epub;
  book = bookIsEpub ? (Book*)&epub : (Book*)&txt;
  bool ok = book->open(s_lib[idx].path);
  if (!ok) { book = nullptr; return false; }
  s_libIdx = idx;
  book->setTier(g_size);
  String fam = prefs.getString("font", "classic");
  book->setFamily(fam.c_str());
  g_fam = CJK.currentFamily();
  return true;
}

// 双缓冲 × 两屏：shown = 屏上当前帧，draw = 正在绘制的下一帧
uint8_t* fbShownL = nullptr;
uint8_t* fbDrawL = nullptr;
uint8_t* fbShownR = nullptr;
uint8_t* fbDrawR = nullptr;
bool dualOk = false;   // 两屏都就绪；false = 单屏退化
bool bookOk = false;   // 书可用；false = 退页码 demo
uint32_t pageCount = 0;    // 刷新次数（管节奏 + 统计）
uint32_t demoPage = 0;     // demo 页号（书不可用时用）
int tmp1 = 0;

static uint8_t* allocFrame() {
  uint8_t* p = (uint8_t*)ps_malloc(EinkPanel::FULL_BYTES);
  if (!p) p = (uint8_t*)malloc(EinkPanel::FULL_BYTES);
  return p;
}

// 本次刷新要显示的左右两页内容：优先书，不可用则退回页码 demo
static void preparePages(bool fullMode) {
  if (bookOk && book && !book->renderPair(fbDrawL, fbDrawR)) {
    Serial.println("WARN: book render failed - fallback to demo pages");
    bookOk = false;
  }
  if (!bookOk) {
    drawDemoPage(fbDrawL, demoPage, "LEFT  (demo)", fullMode);
    drawDemoPage(fbDrawR, demoPage + 1, "RIGHT (demo)", fullMode);
    demoPage += 2;
  }
}

// 一次双屏刷新（右屏没就绪时退单屏）。返回 false = BUSY 超时
static bool refreshPair(EinkPanel::Speed s) {
  if (dualOk) return dual.present(fbShownL, fbDrawL, fbShownR, fbDrawR, s);
  left.setSpeed(s);
  if (!left.showDiff(fbShownL, fbDrawL, false)) return false;
  return left.waitIdle(8000);
}

static void swapPair() {
  uint8_t* t;
  t = fbShownL; fbShownL = fbDrawL; fbDrawL = t;
  t = fbShownR; fbShownR = fbDrawR; fbDrawR = t;
}

// 收割验证入口（定义见下方 handleSerial 区）：刷新/开机噪声窗口只放行
// "恰好一个命令字符 + 换行"形态的用户输入，裸字节/乱码打 hex 丢弃
static void harvestRx(const char* why);

// 一轮：出内容 → 刷新 → 计时统计 → 双缓冲换挡
static void runRefresh(bool fullMode) {
  preparePages(fullMode);

  uint32_t t0 = millis();
  bool ok = refreshPair(fullMode ? EinkPanel::FULL : EinkPanel::FAST);
  uint32_t dt = millis() - t0;

  stats.add(fullMode, dt, ok);
  Serial.printf("%s refresh #%lu%s: %lu ms (%s)\n",
                fullMode ? "full" : "fast", (unsigned long)pageCount,
                dualOk ? " L+R" : " L", (unsigned long)dt,
                ok ? "ok" : "TIMEOUT");
  swapPair();

  // 刷新期间 SPI 灌帧 + 升压高压疑似串扰 RX0（GPIO44），凭空灌进假命令字节
  // （P4 唤醒实测：幽灵 'b'/'h' 都出现在开机窗口；刷新窗口同样设防）。
  // 收割验证：带换行的完整命令（用户在刷新窗口内抢发的 b/f/s/h）入队照常执行，
  // 裸字节/乱码（噪声形态）打十六进制丢弃 —— 防误切书且不吞用户输入
  harvestRx(fullMode ? "full refresh" : "fast refresh");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== dual e-ink book demo ===");

  // 唤醒原因（开盖恢复路径的判定依据；非 EXT1 = 走冷启动 NVS 恢复，会全刷
  // 一次屏。上次唤醒日志里没有 [wake] EXT1 行、cause 未知 → 每次开机都打）
  int wc = (int)esp_sleep_get_wakeup_cause();
  Serial.printf("[wake] cause=%d%s\n", wc,
                wc == (int)ESP_SLEEP_WAKEUP_EXT1 ? " (EXT1: lid)" : " (not EXT1)");
  // 自醒取证（2026-09-28 首次深睡无故自醒，醒后霍尔读开盖 >10s）：
  // ① 睡了多久 —— 秒级 = 入睡瞬间毛刺/电平边际；分钟级 = 盖子真被打开过
  // ② EXT1 触发脚位掩码 —— 确认是 GPIO4 而不是别的脚
  if (wc == (int)ESP_SLEEP_WAKEUP_EXT1)
    Serial.printf("[wake] ext1 pins=0x%llx\n",
                  (unsigned long long)esp_sleep_get_ext1_wakeup_status());
  if (s_sleepAt > 0) {
    struct timeval now;
    if (gettimeofday(&now, nullptr) == 0)
      Serial.printf("[wake] slept %ld s\n", (long)(now.tv_sec - s_sleepAt));
  }

  // 最先释放深睡 hold：EXT1 唤醒/外部 RST 后 hold 残留在 RTC 域，不放开会把
  // SPI 脚锁死在睡眠电平 → 屏 SPI 废掉，恢复渲染/翻页全挂（无 hold 状态下
  // 调用是无害 no-op，上电首跑也安全）
  for (size_t i = 0; i < sizeof(SLEEP_HOLD_PINS) / sizeof(SLEEP_HOLD_PINS[0]); i++)
    gpio_hold_dis((gpio_num_t)SLEEP_HOLD_PINS[i]);
  gpio_deep_sleep_hold_dis();

  pinMode(PIN_HALL, INPUT);   // DRV5032 推挽输出，不需内部上拉
  // 唤醒瞬间的霍尔电平：EXT1 醒 + hall=开盖 = 盖子确实开着（或磁铁没对上）；
  // EXT1 醒 + hall=合盖 = 毛刺自醒实锤（醒来时电平已回落）
  int hl = digitalRead(PIN_HALL);
  Serial.printf("[wake] hall=%d (%s)%s\n", hl,
                hl == HALL_CLOSED_LEVEL ? "closed" : "open",
                (wc == (int)ESP_SLEEP_WAKEUP_EXT1 && hl == HALL_CLOSED_LEVEL)
                    ? " <- EXT1 woke but lid reads CLOSED (spurious?)" : "");
  // total=0 → PSRAM 没初始化（OPI 失败/板型不符）；2097152 → 2MB 板；
  // 8388608 → N16R8 正常。unifont 2MB 装不装得下全看这行
  Serial.printf("[mem] PSRAM total=%u free=%u | heap free=%u\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
                (unsigned)ESP.getFreeHeap());
  prefs.begin("eink-book");

  fbShownL = allocFrame();
  fbDrawL = allocFrame();
  fbShownR = allocFrame();
  fbDrawR = allocFrame();
  if (!fbShownL || !fbDrawL || !fbShownR || !fbDrawR) {
    Serial.println("FATAL: framebuffer alloc failed");
    while (1) delay(1000);
  }

  // 四个缓冲全部填白：上电屏上内容未知，DTM1 按"上一帧全白"喂；同时保证
  // 首帧 DTM2 绝不会把 malloc 垃圾刷上屏（P2 首刷花屏的根因）
  memset(fbShownL, 0xFF, EinkPanel::FULL_BYTES);
  memset(fbDrawL, 0xFF, EinkPanel::FULL_BYTES);
  memset(fbShownR, 0xFF, EinkPanel::FULL_BYTES);
  memset(fbDrawR, 0xFF, EinkPanel::FULL_BYTES);
  Serial.printf("[mem] after fb x4: psram free=%u | heap free=%u\n",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());

  // GUI_Paint 必须先 NewImage：不调用时 Width/Height/Scale 全是 0，
  // Paint_Clear 空转、所有绘制被边界检查丢弃（P1 花屏的根因）
  // 竖版页在这里定方向：显存仍 648×480 横版，逻辑画布 480×648（PAGE_W×PAGE_H），
  // 旋转由 Paint_SetPixel 逐像素完成。之后各画布只 Paint_SelectImage 切换，
  // 宽高/旋转保持这次 NewImage 的设置（几何参数跟着 buffer 走是错的，别改回 0）
  Paint_NewImage(fbShownL, EinkPanel::WIDTH, EinkPanel::HEIGHT, PAGE_ROTATE, WHITE);

  if (!left.begin()) {
    Serial.println("FATAL: left panel begin() BUSY timeout - check wiring");
    while (1) delay(1000);
  }
  if (right.begin()) {
    dualOk = true;
    Serial.println("both panels OK - dual overlapping mode");
  } else {
    Serial.println("WARN: right panel begin() timeout - single screen mode");
  }

  // === P4 书库：扫描 → 按 NVS 存档（无则 test_cn → test → 第一本）===
  scanLibrary();
  g_size = prefs.getUChar("size", TXT_SIZE_PX);
  int startIdx = -1;
  String lastBook = prefs.getString("book", "");
  if (lastBook.length())
    for (int i = 0; i < s_nlib; i++)
      if (lastBook.equals(s_lib[i].path)) { startIdx = i; break; }
  if (startIdx < 0)
    for (int i = 0; i < s_nlib; i++)
      if (strstr(s_lib[i].path, "test_cn.txt")) { startIdx = i; break; }
  if (startIdx < 0)
    for (int i = 0; i < s_nlib; i++)
      if (strstr(s_lib[i].path, "test.txt")) { startIdx = i; break; }
  if (startIdx < 0 && s_nlib > 0) startIdx = 0;

  // 首选书打不开（损坏/被删/存档路径失效）→ 顺位试完书库其余，全失败才退 demo
  bookOk = openBook(startIdx);
  for (int i = 1; !bookOk && i < s_nlib; i++)
    bookOk = openBook((startIdx + i) % s_nlib);
  if (!bookOk) Serial.println("WARN: no book on SD - demo pages only");
  else printFontState();

  // 第 0/1 页：全刷（shown 还是白的）→ 之后 shown 永远 = 屏上真实内容。
  // EXT1 深睡唤醒例外：屏上图像没丢（VCI 常供），把存档对页重画进 fbDraw 再
  // memcpy 到 fbShown —— shown = 屏幕真实内容，**不推送面板** → 恢复零闪烁，
  // 下一翻页的 diff/DTM1 也正确。屏从头到尾一直在显示，"开盖即恢复"
  bool resumed = false;
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("[wake] EXT1 (lid opened) - resuming book");
    bool restored = false;
    if (bookOk && s_sleepFilePos >= 0) {
      if (bookIsEpub)
        restored = s_sleepChapter >= 0 &&
                   epub.restorePair((uint16_t)s_sleepChapter, s_sleepFilePos, s_sleepPage);
      else
        restored = txt.restorePair(s_sleepFilePos, s_sleepPage);
    }
    if (restored) {
      preparePages(true);
      memcpy(fbShownL, fbDrawL, EinkPanel::FULL_BYTES);
      memcpy(fbShownR, fbDrawR, EinkPanel::FULL_BYTES);
      resumed = true;
      Serial.printf("[wake] page %lu re-rendered into RAM, panel untouched\n",
                    (unsigned long)s_sleepPage);
    }
  }
  if (!resumed && bookOk) {
    // 冷启动：按 NVS 进度恢复（上次读到哪从哪开始；无存档 = 第 0 页）
    long pos = prefs.getLong("pos", -1);
    uint32_t pg = prefs.getUInt("page", 0);
    int16_t ch = prefs.getShort("ch", -1);
    bool restored = false;
    if (pos >= 0) {
      if (bookIsEpub)
        restored = ch >= 0 && epub.restorePair((uint16_t)ch, pos, pg);
      else
        restored = txt.restorePair(pos, pg);
    }
    if (!restored) book->restartPair();   // 无有效存档：从第 0 对页开始
  }
  if (!resumed) runRefresh(true);
  // setup 期间（面板初始化/章节解压/全刷升压）攒下的 RX 字节过一遍账再进 loop：
  // 收割验证 —— 用户开机电前抢发的完整命令（"b\n"）入队补执行，裸字节当噪声丢弃
  // （P4 幽灵 'b'/'h' 实际就在这个窗口入栈）
  harvestRx("boot");
}

// 合盖检测：稳定在合盖电平 300ms（防抖）才返回 true
static bool hallClosing() {
  static uint32_t since = 0;
  if (digitalRead(PIN_HALL) != HALL_CLOSED_LEVEL) { since = 0; return false; }
  if (!since) { since = millis(); return false; }
  return millis() - since >= 300;
}

// 合盖 → 存对页位置 → SPI 引脚 hold → EXT1(开盖) 深睡。不返回
static void enterDeepSleep() {
  saveProgress();
  s_sleepFilePos = bookOk ? book->pairFilePos() : -1;
  s_sleepPage    = bookOk ? book->pairPageNo() : 0;
  s_sleepChapter = (bookOk && bookIsEpub) ? (int16_t)epub.pairChapter() : (int16_t)-1;
  Serial.printf("[%lu ms] lid closed -> deep sleep (pos=%ld page=%lu ch=%d)\n",
                (unsigned long)millis(), s_sleepFilePos, (unsigned long)s_sleepPage,
                (int)s_sleepChapter);
  if (Serial) Serial.flush();
  {   // 记录入睡 wall-clock（RTC 时钟深睡期间照走，醒后算实际睡眠时长）
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) == 0) s_sleepAt = tv.tv_sec;
  }

  gpio_deep_sleep_hold_en();
  for (size_t i = 0; i < sizeof(SLEEP_HOLD_PINS) / sizeof(SLEEP_HOLD_PINS[0]); i++) {
    esp_err_t e = gpio_hold_en((gpio_num_t)SLEEP_HOLD_PINS[i]);
    if (e != ESP_OK)
      Serial.printf("WARN: gpio_hold_en(%d) err=%d\n", SLEEP_HOLD_PINS[i], (int)e);
  }
#if HALL_CLOSED_LEVEL == 0
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_HALL, ESP_EXT1_WAKEUP_ANY_HIGH);
#else
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_HALL, ESP_EXT1_WAKEUP_ALL_LOW);
#endif
  esp_deep_sleep_start();
}

// 字体/字号切换后：同一对页按新字体立即重排（Kindle 式切换即预览）
static void previewFontChange() {
  if (bookOk && book->restartPair()) runRefresh(false);
}

// 切下一本书：关当前 → 开下一本 → 从第 0 页起（进度已随切书落盘）
static void nextBook() {
  if (s_nlib <= 1) return;
  saveProgress();
  int idx = (s_libIdx + 1) % s_nlib;
  if (!openBook(idx)) {
    Serial.println("WARN: next book open failed - keeping demo");
    bookOk = false;
    return;
  }
  bookOk = true;
  prefs.remove("pos"); prefs.remove("page"); prefs.remove("ch");  // 新书从第 0 页起
  saveProgress();
  printFontState();
  runRefresh(true);
}

// ===== 串口输入：命令分发 + 噪声窗口"收割验证" ===============================
// 串口命令：f = 切字体家族（SD 包自动发现），s = 切字号 16/24/32，
// b = 切下一本书，h = 状态（P6 4 键落地后绑同一套接口）
// 三个输入窗口分开处理（P4 定案 2026-09-28）：
//   · 等待期（5s 面板空闲，用户输入主窗口）→ handleSerial 每 20ms 轮询即时执行。
//     原来只在等待前读一次，等待期收到的字节滞留到本轮刷新后被 drain 吞掉
//     （实测 7 个 'b' 全被吞 = "发 b 不能切书"的根因）
//   · 刷新期/开机期（SPI+升压窗口，幽灵字节唯一证据来源：开机窗口入栈的裸
//     'b'/'h'）→ harvestRx 收割：只有"恰好一个命令字符 + 换行"（终端实测发
//     62 0a）才入队放行，裸字节/乱码打 hex 丢弃 —— 用户输入不丢，噪声过不去
static char g_rxQueue[4];   // 收割验证通过、待执行的命令
static int g_rxN = 0;

static void execSerialCmd(char c) {
  if (c == 'f' && CJK.familyCount() > 1) {
    g_fam = (g_fam + 1) % CJK.familyCount();
    if (book) book->setFamily(CJK.familyName(g_fam));
    g_fam = CJK.currentFamily();              // 包装载失败会自动回 classic
    prefs.putString("font", CJK.familyName(g_fam));
    printFontState();
    previewFontChange();
  } else if (c == 's') {
    g_size = (g_size == 16) ? 24 : (g_size == 24 ? 32 : 16);
    if (book) book->setTier(g_size);
    g_fam = CJK.currentFamily();
    prefs.putUChar("size", g_size);
    printFontState();
    previewFontChange();
  } else if (c == 'b') {
    nextBook();
  } else if (c == 'h') {
    printFontState();
  }
}

// 收割当前 RX 缓冲（刷新后/开机时调用）：验证通过入队，否则打日志丢弃
static void harvestRx(const char* why) {
  if (!Serial.available()) return;
  char in[32];
  int n = 0, c;
  while (n < (int)sizeof(in) && (c = Serial.read()) >= 0) in[n++] = (char)c;
  int cmds = 0;
  char cmd = 0;
  bool nl = false;
  for (int i = 0; i < n; i++) {
    if (in[i] == '\n' || in[i] == '\r') { nl = true; continue; }
    cmd = in[i]; cmds++;
  }
  // 纯换行 = 用户命令的尾字节（b 被等待窗读走执行、\n 滞留进刷新窗口），
  // 不是噪声也不是命令 —— 单独记一笔，别混进 dropped 里误导排查
  if (cmds == 0 && nl) {
    Serial.printf("[rx] ignore %d newline byte(s) after %s\n", n, why);
    return;
  }
  if (cmds == 1 && nl && g_rxN < (int)sizeof(g_rxQueue)) {
    g_rxQueue[g_rxN++] = cmd;
    Serial.printf("[rx] '%c' queued after %s\n", cmd, why);
    return;
  }
  char hex[72] = "";
  int k = 0;
  for (int i = 0; i < n && k < (int)sizeof(hex) - 4; i++)
    k += snprintf(hex + k, sizeof(hex) - k, "%02x ", (uint8_t)in[i]);
  Serial.printf("[rx] dropped %d byte(s) after %s: %s\n", n, why, hex);
}

// 命令入口：先执行收割队列（较早到达），再读实时字节（等待期窗口）
static void handleSerial() {
  while (g_rxN > 0) {
    char c = g_rxQueue[0];
    g_rxN--;
    memmove(g_rxQueue, g_rxQueue + 1, g_rxN);
    Serial.printf("[rx] exec '%c' (queued)\n", c);
    execSerialCmd(c);
  }
  while (Serial.available()) {
    int raw = Serial.read();
    if (raw < 0) break;
    char c = (char)raw;
    Serial.printf("[rx] cmd 0x%02x ('%c')\n", raw,
                  (raw >= 32 && raw < 127) ? c : '?');
    execSerialCmd(c);
  }
}

void loop() {
  // 分片等待下一次翻页，每 20ms 查一次霍尔 + 轮询一次串口：
  //   · 合盖 300ms 防抖 → 深睡（不返回）；原来整段 delay(5s) 期间是聋的
  //   · 串口必须在等待期内轮询（P4 回归根因）：原来 handleSerial 只在 loop 顶部
  //     跑一次，等待期（5s，用户输入主窗口）收到的字节滞留到本轮刷新结束才被
  //     收割窗口碰到 —— 7 个 'b\n' 全被当噪声吞掉，"b 切书"100% 失效
  uint32_t waitStart = millis();
  while (millis() - waitStart < PAGE_INTERVAL_MS) {
    handleSerial();
    if (hallClosing()) enterDeepSleep();   // 不返回
    delay(20);
  }
  if (hallClosing()) enterDeepSleep();

  pageCount++;
  runRefresh(pageCount % (FULL_EVERY + 1) == 0);
  if (pageCount % SAVE_EVERY_PAIRS == 0) saveProgress();   // 周期进度落盘
  if (pageCount % 100 == 0) stats.printSummary(pageCount, FAST_TARGET_MS);
}
