// 双屏电子书验证程序（ESP32-S3 N16R8，见 DESIGN.md）
//
// 职责分层（本文件只做组装 + 刷新节奏，逻辑都在 src/）：
//   EinkPanel  单屏驱动（UC8179，三档速度，DTM1 写真实上一帧）
//   DualEink   双屏重叠刷新 present()：L 灌帧→DRF→趁 L busy 给 R 灌帧→DRF→等两屏
//   Storage    SDMMC 1-bit @20MHz 挂载（正式引脚 38/39/40）
//   Book       统一书接口（TxtBook=TXT / EpubBook=EPUB，P4 起书库多书切换）
//   EpubImg    EPUB 插图解码（JPEG=S3 ROM 硬件解码器 / PNG=PNGdec，Bayer 抖动落画布）
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
//
// 系统菜单（NetSvc + SysMenu，DESIGN §8）：串口 m 开/关 —— 左页菜单右页状态，
// WiFi 配置（热点配网页）/ WiFi 开关 / OTA 开关 / 网页上传电子书；
// 菜单开着暂停自动翻页，合盖仍深睡（开盖回书页，菜单不跨睡）。
//
// P6 板载 4 键（src/Keys.h，GPIO1/2/9/10，电气台架 key_test 已过 2026-10-08）：
//   Prev/Next = 上/下一对页（长按 250ms 后 300ms 连发，历史栈 48 对页可回退）、
//   Menu 短按 = **主页（选书）** / 长按 250ms = 系统菜单、
//   Font 短按 = 字号 / 长按 = 字体家族；
//   自动翻页**默认关**（真实阅读不自翻，串口 a 开 = 台架测试）。
//   按键不参与深睡唤醒（唤醒只有霍尔 EXT1，2026-10-08 定案）。
//
// 主页（P7，src/HomePage.*）：左页书库列表（选中 '>' + [在读] 标记），右页
// 正在读/书库统计/按键说明；Font 短按 = 打开选中书（选中当前读的那本 = 只回
// 阅读页）；主页里 Prev/Next 上下选。串口同款：n 进出主页、j/k 选、空格打开。
// 与系统菜单分层：菜单 = 设置（WiFi/OTA/上传），主页 = 选书；退出菜单按来源
// 回主页或书页。合盖深睡记住"睡前屏上是主页还是书页"，开盖原样恢复。

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
#include "src/NetSvc.h"
#include "src/SysMenu.h"
#include "src/HomePage.h"
#include "src/Keys.h"
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
RTC_DATA_ATTR bool s_sleepMenu = false;      // 入睡时菜单还开着：屏上是菜单不是书页，
                                             // 开盖跳过零闪烁恢复、走全刷回书页
RTC_DATA_ATTR bool s_sleepHome = false;      // 入睡时主页开着：屏上是选书页 → 开盖
                                             // 走全刷回主页（书的进度已按 NVS 恢复）

static const uint32_t FULL_EVERY = 5;        // 每 5 次快刷插 1 次全刷
static const uint32_t PAGE_INTERVAL_MS = 5000;
static const uint32_t FAST_TARGET_MS = 1800; // 验收线：整页 ≤1.8s
static const uint32_t SAVE_EVERY_PAIRS = 10; // 进度落盘频率（对页数）

// P6（2026-10-08 定案）：自动翻页**默认关** —— 有按键后真实阅读不能自翻；
// 台架测试（P5 50 页/P2 100 次连刷/电量）用串口 a 临时打开
static bool g_autoFlip = false;

// 主页（P7 选书页）激活中：屏上是主页而非书页 → 跳过自动翻页、Prev/Next 改为
// 选书、Menu 短按已在主页时不动。系统菜单与主页可叠（菜单优先分发），
// 退出菜单按 g_home 回主页或书页
static bool g_home = false;

// P6 上一页的历史栈（函数见 setup 前的"按键翻页"块）：开机恢复点之前的页
// 不可回退 → 换书/开书即清零（见 openBook）
struct HistEnt { long pos; uint32_t page; int16_t ch; };
static HistEnt s_hist[48];
static int s_histN = 0;
static const int HIST_MAX = (int)(sizeof(s_hist) / sizeof(s_hist[0]));

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
  Serial.printf("[font] family=%s size=%u book=%s (serial: f=font s=size b=next book n=home m=menu a=auto h=status)\n",
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
  s_histN = 0;                 // 换书：旧书的对页坐标对新书无效（P6 历史栈）
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

// 一轮刷新的通用节奏：计时统计 → 灌帧刷新 → 双缓冲换挡 → 收割 RX。
// 前提：内容已画进 fbDrawL/fbDrawR。tag 记日志（"menu"/"home"…），
// tag 同时是收割窗口的 why —— 刷新期 SPI 灌帧会灌进假命令字节（见 runRefresh）
static void presentDraw(bool fullMode, const char* tag) {
  uint32_t t0 = millis();
  bool ok = refreshPair(fullMode ? EinkPanel::FULL : EinkPanel::FAST);
  uint32_t dt = millis() - t0;

  stats.add(fullMode, dt, ok);
  Serial.printf("%s %s refresh: %lu ms (%s)\n", tag,
                fullMode ? "full" : "fast", (unsigned long)dt,
                ok ? "ok" : "TIMEOUT");
  swapPair();

  char why[32];
  snprintf(why, sizeof(why), "%s refresh", tag);
  harvestRx(why);
}

// 一次菜单刷新（内容 = menu.draw 进 draw 缓冲）。
// fullMode：开菜单用全刷清残影；导航/动作用快刷（DTM1 = 屏上真实的菜单帧）
static void showMenuRefresh(bool fullMode) {
  menu.draw(fbDrawL, fbDrawR, s_nlib);
  presentDraw(fullMode, "menu");
}

// ===== 主页（P7 选书页，src/HomePage.*）==============================
// 拼书库上下文 → home.draw 进 draw 缓冲 → 刷一轮。
// 打开主页用全刷（书页 → 主页整屏换内容，清残影）；选书/滚动用快刷
static void drawHomeRefresh(bool fullMode) {
  const char* paths[sizeof(s_lib) / sizeof(s_lib[0])];
  for (int i = 0; i < s_nlib; i++) paths[i] = s_lib[i].path;
  char fontInfo[40];
  snprintf(fontInfo, sizeof(fontInfo), "%s %upx", CJK.familyName(g_fam), g_size);

  HomePage::Ctx ctx;
  ctx.paths = paths;
  ctx.count = s_nlib;
  ctx.curIdx = s_libIdx;
  ctx.curPage = (bookOk && book) ? (long)book->pairPageNo() : -1L;
  ctx.fontInfo = fontInfo;

  home.draw(fbDrawL, fbDrawR, ctx);
  presentDraw(fullMode, "home");
}

// 系统菜单入口（Menu 长按 / 串口 m）：全刷清上一帧残影。
// 主页开着时 g_home 不清 —— 退出菜单（applyMenuResult CLOSE）据此回主页
static void openSysMenu() {
  if (menu.isOpen()) return;
  menu.open();
  showMenuRefresh(true);
}

// 主页入口（Menu 短按 / 串口 n）：选中项落到正在读的那本
static void openHome() {
  if (g_home || menu.isOpen()) return;
  home.open(s_libIdx, s_nlib);
  g_home = true;
  drawHomeRefresh(true);                 // 全刷：整屏换内容
}

// 回阅读页：restartPair 把当前对页重画进 draw 缓冲（不推进页码），
// 失败（demo 页/首渲染前）自然走下一对页 —— 与菜单退出同一条路径
static void closeHome() {
  if (!g_home) return;
  g_home = false;
  home.close();                  // m_open 同步收掉（PICK 路径不经过 home.key 的
                                 // 自闭分支，不收会留下 isOpen()=true 的悬置态）
  if (bookOk && book) book->restartPair();
  runRefresh(false);
}

// 打开选中书。选中正在读的那本 = 只回阅读页（进度原地不动，不重开）；
// 换书 = 关旧开新、从第 0 对页起（单槽进度存档随后被新书覆盖，同串口 b）
static void pickBook(int idx) {
  if (idx == s_libIdx && bookOk && book) { closeHome(); return; }
  if (idx < 0 || idx >= s_nlib) { closeHome(); return; }

  saveProgress();
  if (!openBook(idx)) {
    Serial.println("WARN: home pick open failed - keeping demo");
    bookOk = false;
    closeHome();
    return;
  }
  bookOk = true;
  prefs.remove("pos"); prefs.remove("page"); prefs.remove("ch");
  saveProgress();
  printFontState();
  g_home = false;
  runRefresh(true);                      // 全刷进新书第 0 对页
}

// 主页按键结果 → 刷屏节奏（与 applyMenuResult 对称）
static void applyHomeResult(HomePage::Result r) {
  if (r == HomePage::REDRAW) drawHomeRefresh(false);   // 上下选：快刷
  else if (r == HomePage::CLOSE) closeHome();
  else if (r == HomePage::PICK) pickBook(home.selected());
}

// 网页上传完成（NetSvc 回调，loop 上下文）：重扫书库。
// 覆盖的正是正在读的书 → 必须先关旧句柄再重开（新内容从第 0 对页起，旧进度作废）；
// 否则重扫后 s_lib 下标可能位移，把当前书的下标找回来
static void onBookAdded(const char* path) {
  bool wasCur = book && s_libIdx >= 0 && !strcmp(path, s_lib[s_libIdx].path);
  char cur[64] = "";
  if (!wasCur && book && s_libIdx >= 0)
    snprintf(cur, sizeof(cur), "%s", s_lib[s_libIdx].path);
  if (wasCur) { book->close(); book = nullptr; bookOk = false; }

  scanLibrary();

  if (wasCur) {
    int idx = -1;
    for (int i = 0; i < s_nlib; i++)
      if (!strcmp(path, s_lib[i].path)) { idx = i; break; }
    bookOk = idx >= 0 && openBook(idx);
    // 内容被换掉，旧进度对新文件没意义 → 清掉（重开本就从第 0 对页起）
    if (bookOk) { prefs.remove("pos"); prefs.remove("page"); prefs.remove("ch"); }
    Serial.printf("[up] current book replaced, reopen %s\n", bookOk ? "OK" : "FAILED");
  } else if (cur[0]) {
    s_libIdx = -1;
    for (int i = 0; i < s_nlib; i++)
      if (!strcmp(cur, s_lib[i].path)) { s_libIdx = i; break; }
  }
  // 菜单/主页开着：右页"书库: N 本"、左页列表都可能过期 → 重画
  if (menu.isOpen()) showMenuRefresh(false);
  else if (g_home) drawHomeRefresh(false);
}

// ===== P6 按键翻页：对页历史栈 + 统一刷新节奏 ==========================
// 上一页没有"文件里的反向游标"（Book 接口只前进），用历史栈记本次运行访问过
// 的对页坐标（pos/page/ch），Prev 逐条回退 → restorePair 定位 → renderPair 重画。
// 深栈 48 对页 = 上限；开机恢复点之前的页不可回退（历史从空开始，正确语义）。
static void histPush() {
  if (!bookOk || !book) return;
  if (book->pairFilePos() < 0) return;     // 首次渲染前没有可回退坐标
  if (s_histN >= HIST_MAX) {              // 满：丢最旧（回退深度固定 48 对页）
    memmove(&s_hist[0], &s_hist[1], sizeof(s_hist) - sizeof(s_hist[0]));
    s_histN--;
  }
  s_hist[s_histN].pos = book->pairFilePos();
  s_hist[s_histN].page = book->pairPageNo();
  s_hist[s_histN].ch = bookIsEpub ? (int16_t)epub.pairChapter() : (int16_t)-1;
  s_histN++;
}

static bool histPop(HistEnt& out) {
  if (s_histN <= 0) return false;
  out = s_hist[--s_histN];
  return true;
}

// 按存档坐标定位到某一对页（不渲染；调用方随后 runRefresh 重画同一对页）。
// 开机冷恢复、深睡唤醒恢复、按键回退三条路径共用
static bool restoreAt(long pos, uint32_t page, int16_t ch) {
  if (!bookOk || !book || pos < 0) return false;
  return bookIsEpub ? (ch >= 0 && epub.restorePair((uint16_t)ch, pos, page))
                    : txt.restorePair(pos, page);
}

// 一轮翻页的节奏：计数 → 刷新（5 快 1 全）→ 周期落盘 → 每 100 次统计。
// 原 loop 尾部的自动翻页逻辑抽出：自动翻页 / 按键翻页共用
static void refreshOnce() {
  pageCount++;
  runRefresh(pageCount % (FULL_EVERY + 1) == 0);
  if (pageCount % SAVE_EVERY_PAIRS == 0) saveProgress();
  if (pageCount % 100 == 0) stats.printSummary(pageCount, FAST_TARGET_MS);
}

// 翻 delta 对页：+1 下一对 / -1 上一对 / |delta|>1 一次刷新跨多页。
// 正向：renderPair 每调一次推进一对；中间页画进 draw 缓冲会被覆盖，但每页
//   都进历史栈 → 回退仍能一页页走回来。
// 负向：从历史栈连 pop|delta| 条，恢复最深那条（被跳过的中间页靠 Next 重走）。
static void turnPage(int delta) {
  if (delta == 0) return;
  if (delta > 8) delta = 8;               // 事件风暴上限（刷新 1.3s，攒不出更多）
  if (delta < -8) delta = -8;

  if (!bookOk) {                          // 无书：页码 demo（回退 clamp 到 0）
    // 正向：preparePages 画完自己 +2，这里只补"跨过的"中间对页（delta=1 → +0，
    // 与原来行为一致）；负向：直接往回减
    long np = (long)demoPage + (delta > 0 ? 2L * (delta - 1) : 2L * delta);
    demoPage = np < 0 ? 0 : (uint32_t)np;
    refreshOnce();
    return;
  }

  if (delta > 0) {
    for (int k = 0; k < delta; k++) {
      histPush();                         // 先记"当前显示的那对"再前进
      if (k < delta - 1 && !book->renderPair(fbDrawL, fbDrawR)) {
        Serial.println("WARN: skip render failed - stop skipping");
        break;                            // 剩余交给 preparePages（退 demo 或报错）
      }
    }
  } else {
    HistEnt h;
    bool any = false;
    for (int k = 0; k < -delta && histPop(h); k++) any = true;
    if (!any) {                           // 历史空（开机恢复点）：无处可退，不刷屏
      Serial.println("[key] prev: no history (boot restore point)");
      return;
    }
    if (!restoreAt(h.pos, h.page, h.ch)) {
      Serial.println("WARN: prev restore failed - re-render current pair");
      book->restartPair();                // 兜底：原地重画，不推进页码
    }
  }
  refreshOnce();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== dual e-ink book demo ===");

  // RTC 跨醒保留：上次入睡时菜单还开着（屏上是菜单帧而非书页）→ 本醒不能走
  // "零闪烁恢复"（fbShown 与屏上内容对不上，diff 会花），落到下方全刷回书页。
  // 无条件清零：这标志只在入睡瞬间写一次
  bool sleptWithMenu = s_sleepMenu;
  s_sleepMenu = false;
  // 同上：睡前屏上是主页（选书页）→ 本醒不能走零闪烁恢复，落全刷回主页
  bool sleptWithHome = s_sleepHome;
  s_sleepHome = false;

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
  // 4 键（P6）：先挂边沿中断再进任何长刷新，刷新窗口里的按压才有 ISR 记账。
  // 醒后/开机时已被按住的键由 Keys 忽略（不补发按下，见 src/Keys.h）
  keys.begin();
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

  // 系统网络服务（菜单后端）：自带 NVS 命名空间 "eink-net"，WiFi 默认关
  // （DESIGN §2 省电定案；菜单里手动开）。上传完成回调重扫书库
  net.onBookAdded = onBookAdded;
  net.begin();

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
  if (sleptWithMenu)
    Serial.println("[wake] slept with menu open - full refresh back to book page");
  if (sleptWithHome)
    Serial.println("[wake] slept on home page - full refresh back to home");
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
      !sleptWithMenu && !sleptWithHome) {
    Serial.println("[wake] EXT1 (lid opened) - resuming book");
    bool restored = false;
    if (s_sleepFilePos >= 0)
      restored = restoreAt(s_sleepFilePos, s_sleepPage, s_sleepChapter);
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
    if (pos >= 0) restored = restoreAt(pos, pg, ch);
    if (!restored) book->restartPair();   // 无有效存档：从第 0 对页开始
  }
  if (!resumed) {
    if (sleptWithHome) {
      // 睡前在主页：进度已在上面按 NVS 恢复（选中当前书即可接着读），
      // 屏上重画主页（全刷 —— fbShown 是白的，不能走零闪烁）
      home.open(s_libIdx, s_nlib);
      g_home = true;
      drawHomeRefresh(true);
    } else {
      runRefresh(true);
    }
  }
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
  s_sleepMenu = menu.isOpen();   // 屏上是菜单帧 → 记账，开盖走全刷回书页
  s_sleepHome = g_home;          // 屏上是主页帧 → 记账，开盖走全刷回主页
  net.beforeSleep();             // 停 WebServer/OTA、关 WiFi 射频（深睡不带电）
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
// b = 切下一本书，m = 系统菜单，n = 主页（选书），h = 状态
//（P6 4 键落地后绑同一套接口：Menu 短按 = n / 长按 = m）
// 三个输入窗口分开处理（P4 定案 2026-09-28）：
//   · 等待期（5s 面板空闲，用户输入主窗口）→ handleSerial 每 20ms 轮询即时执行。
//     原来只在等待前读一次，等待期收到的字节滞留到本轮刷新后被 drain 吞掉
//     （实测 7 个 'b' 全被吞 = "发 b 不能切书"的根因）
//   · 刷新期/开机期（SPI+升压窗口，幽灵字节唯一证据来源：开机窗口入栈的裸
//     'b'/'h'）→ harvestRx 收割：只有"恰好一个命令字符 + 换行"（终端实测发
//     62 0a）才入队放行，裸字节/乱码打 hex 丢弃 —— 用户输入不丢，噪声过不去
static char g_rxQueue[4];   // 收割验证通过、待执行的命令
static int g_rxN = 0;

// 菜单动作结果 → 刷屏节奏（串口命令与板载按键两条入口共用）
static void applyMenuResult(SysMenu::Result r) {
  if (r == SysMenu::REDRAW) {
    showMenuRefresh(false);        // 导航/动作：快刷（DTM1 = 屏上真实菜单帧）
  } else if (r == SysMenu::CLOSE) {
    // 退出菜单 → 按来源还原：从主页开的回主页（快刷），否则回书页
    // （restartPair 把当前对页重画进 draw 缓冲，不推进页码；
    //   失败（demo 页/首渲染前）就自然走下一对页）
    if (g_home) { drawHomeRefresh(false); return; }
    if (bookOk && book) book->restartPair();
    runRefresh(false);
  }
}

static void execSerialCmd(char c) {
  // 菜单打开：字节全部交给菜单（j/k 导航、Enter/空格执行、q/m 退出；
  // ESC 等杂字节被 key() 吞掉）—— f/s/b/h/a 在菜单期间刻意不响应
  if (menu.isOpen()) {
    applyMenuResult(menu.key(c));
    return;
  }
  // 主页打开：字节交给主页（j/k 选书、空格/回车打开、q/n 回书页；
  // f/s/b/h/a 在主页期间刻意不响应），m 仍是系统菜单
  if (g_home) {
    if (c == 'm') { openSysMenu(); return; }
    applyHomeResult(home.key(c));
    return;
  }
  if (c == 'm') {
    openSysMenu();                   // 全刷，清上一帧残影
    return;
  }
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
  } else if (c == 'n') {
    openHome();                      // 主页（= Menu 短按；n = 再按一次回书页）
  } else if (c == 'b') {
    nextBook();
  } else if (c == 'h') {
    printFontState();
  } else if (c == 'a') {                       // 自动翻页开关（P6 默认关）
    g_autoFlip = !g_autoFlip;
    Serial.printf("[auto] auto page flip %s (interval %lu ms)\n",
                  g_autoFlip ? "ON" : "OFF", (unsigned long)PAGE_INTERVAL_MS);
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

// ===== P6 板载 4 键分发（电气台架 key_test 已过，2026-10-08）==========
//   书页：Prev/Next = 上/下一对页（长按 250ms 后每 300ms 连发）、
//         Menu 短按 = 主页（选书，src/HomePage）/ 长按 250ms = 系统菜单、
//         Font 短按 = 字号(s) / 长按 = 字体家族(f)
//   主页：Prev/Next = 上下选书（连发=滚动）、Font 短按 = 打开选中书、
//         Menu 短按 = 已在主页（不动）、长按 = 系统菜单
//   菜单内：Prev/Next = 上下（连发=滚动）、Font 短按 = 执行、Menu = 退出
//   睡着时按过的键：Keys 不补发按下 → 消费端再挡一次（sawPress）
static bool g_keyDown[Keys::COUNT] = {false};   // RELEASE 前必须见过 PRESS
// 菜单内按 Menu 退出的那一次按住：菜单在 PRESS 就关了，这次按住剩下的
// LONG/RELEASE 必须吞掉 —— 否则 LONG 会再开一次菜单、RELEASE 会当成短按回主页
static bool g_menuExitSwallow = false;

static void keyAction(uint8_t id, uint8_t ev, bool longFired, bool sawPress) {
  if (menu.isOpen()) {
    if ((id == Keys::PREV || id == Keys::NEXT) &&
        (ev == Keys::PRESS || ev == Keys::REPEAT))
      applyMenuResult(menu.key(id == Keys::PREV ? 'k' : 'j'));
    else if (id == Keys::FONT && ev == Keys::RELEASE && !longFired && sawPress)
      applyMenuResult(menu.key(' '));            // 短按 = 执行当前项
    else if (id == Keys::MENU && ev == Keys::PRESS) {
      g_menuExitSwallow = true;                  // 见上方注释
      applyMenuResult(menu.key('q'));            // 再按菜单键 = 退出
    }
    return;                                      // LONG/REPEAT/多余 RELEASE 吞掉
  }
  if (id == Keys::MENU) {
    // 一次按住在 PRESS 时不表态，等 LONG（长按到点）或 RELEASE（松开）再分岔：
    // RELEASE 带 longFired 就能区分短按/长按，不必在按下时抢开菜单
    if (ev == Keys::RELEASE) {
      if (g_menuExitSwallow) { g_menuExitSwallow = false; return; }
      if (!longFired && sawPress && !g_home) openHome();   // 短按 = 主页
      return;
    }
    if (ev == Keys::LONG) {                      // 长按 = 系统菜单
      if (!g_menuExitSwallow) openSysMenu();
      return;
    }
    if (ev == Keys::PRESS) g_menuExitSwallow = false;   // 新一轮按住，清上轮标记
    return;                                      // 等 LONG / RELEASE
  }
  if (g_home) {                                  // 主页：Font 短按 = 打开选中书
    if (id == Keys::FONT && ev == Keys::RELEASE && !longFired && sawPress)
      applyHomeResult(home.key(' '));
    return;                                      // Prev/Next 在 drainKeys 里并批
  }
  if (id == Keys::FONT) {
    if (ev == Keys::LONG) execSerialCmd('f');                        // 长按 = 家族
    else if (ev == Keys::RELEASE && !longFired && sawPress)
      execSerialCmd('s');                                            // 短按 = 字号
    // PREV/NEXT 的翻页在 drainKeys 里并批，这里到此为止（RELEASE 无动作）
  }
}

// 攒一批按键事件一次消费：连续的翻页事件（PRESS/LONG/REPEAT）合并成净位移 ——
// 一次刷新 1.3s，期间攒下的按压只刷一次屏（连按 3 下 = 跨 3 页一次刷完）。
// 主页开着时同一批并成一次选书位移（home.move），菜单内逐条交给 menu.key
static void drainKeys() {
  Keys::Event ev[8];
  int n = 0;
  keys.poll();
  while (n < (int)(sizeof(ev) / sizeof(ev[0])) && keys.take(ev[n])) n++;

  for (int i = 0; i < n;) {
    if (ev[i].ev == Keys::PRESS) g_keyDown[ev[i].id] = true;

    // 每个事件现读菜单状态：同批里 MENU 刚开/关了菜单，后续翻页事件要跟上
    bool menuOpen = menu.isOpen();
    bool pageKey = ev[i].id == Keys::PREV || ev[i].id == Keys::NEXT;
    bool pageEv = ev[i].ev == Keys::PRESS || ev[i].ev == Keys::LONG ||
                  ev[i].ev == Keys::REPEAT;
    if (!menuOpen && pageKey && pageEv) {
      int delta = 0, j = i;
      // 翻页键的 RELEASE 一并吸收（不清批、不翻页）：连按 N 下产生的是
      // PRESS,RELEASE 交替序列，RELEASE 断批会让每次点按各刷一次屏 ——
      // 吸收后同一刷新窗口内的连按才真正并成一次刷新（本块头注释的承诺）
      while (j < n && (ev[j].id == Keys::PREV || ev[j].id == Keys::NEXT)) {
        if (ev[j].ev == Keys::RELEASE) {
          g_keyDown[ev[j].id] = false;
          j++;
          continue;
        }
        // 事件枚举仅 PRESS/LONG/REPEAT/RELEASE 四种，到这里的都算翻页
        if (ev[j].ev == Keys::PRESS) g_keyDown[ev[j].id] = true;
        delta += (ev[j].id == Keys::NEXT) ? 1 : -1;
        j++;
      }
      if (delta) {
        if (g_home) {
          // 主页：同一批并成一次选中位移（连按/连发 = 一次快刷到位）
          Serial.printf("[key] home select %+d from %d event(s) [bnc=%lu ovf=%lu]\n",
                        delta, j - i, (unsigned long)keys.bounce(),
                        (unsigned long)keys.overflow());
          applyHomeResult(home.move(delta));
        } else {
          Serial.printf("[key] turn %+d pair(s) from %d event(s) [bnc=%lu ovf=%lu]\n",
                        delta, j - i, (unsigned long)keys.bounce(),
                        (unsigned long)keys.overflow());
          turnPage(delta);
        }
      }
      i = j;
      continue;
    }

    bool lf = ev[i].longFired;
    bool sawPress = g_keyDown[ev[i].id];
    if (ev[i].ev == Keys::RELEASE) g_keyDown[ev[i].id] = false;
    keyAction(ev[i].id, ev[i].ev, lf, sawPress);
    i++;
  }
}

void loop() {
  // 分片等待，每 20ms 一轮：按键事件 → 串口 → 网络 → 霍尔。
  //   · 自动翻页默认关（P6 定案 2026-10-08，g_autoFlip，串口 a 开）——
  //     开着时每 PAGE_INTERVAL_MS 自动出下一对页（台架测试用）
  //   · 合盖 300ms 防抖 → 深睡（不返回）
  //   · 串口必须在等待期内轮询（P4 回归根因）：原来 handleSerial 只在 loop 顶部
  //     跑一次，等待期收到的字节滞留到本轮刷新结束才被收割窗口碰到
  //     —— 7 个 'b\n' 全被当噪声吞掉，"b 切书"100% 失效
  uint32_t waitStart = millis();
  while (true) {
    drainKeys();                     // 按键：poll 出事件 → 合并 → 翻页/菜单/字号
    handleSerial();
    net.loop();                      // HTTP 分发 + OTA handle（只在等待窗跑，
                                     // 上传/刷固件的闪存写全落在翻页间隙）
    if (hallClosing()) enterDeepSleep();   // 不返回
    if (g_autoFlip && millis() - waitStart >= PAGE_INTERVAL_MS) break;
    delay(20);
  }
  if (hallClosing()) enterDeepSleep();

  // 菜单/主页开着：屏保持该 UI，跳过自动翻页（下一轮照常轮询串口/网络/霍尔）
  if (menu.isOpen() || g_home) return;

  refreshOnce();
}
