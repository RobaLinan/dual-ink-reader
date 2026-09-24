// 双屏电子书验证程序（ESP32-S3 N16R8，见 DESIGN.md）
//
// 职责分层（本文件只做组装 + 刷新节奏，逻辑都在 src/）：
//   EinkPanel  单屏驱动（UC8179，三档速度，DTM1 写真实上一帧）
//   DualEink   双屏重叠刷新 present()：L 灌帧→DRF→趁 L busy 给 R 灌帧→DRF→等两屏
//   Storage    SDMMC 1-bit @20MHz 挂载（正式引脚 38/39/40）
//   TxtBook    TXT 打开 + 流式分页渲染（左 N / 右 N+1，按词断行）
//   PageStats  刷新耗时统计（min/avg/max + 超时，每 100 页汇总）
//   DemoPage   兜底页码页（SD/TXT 不可用时）
//
// 节奏：每 5 次快刷插 1 次全刷（DESIGN §5.9 = Diptyx partialUpdatesRemaining=5）；
// 降级链：右屏超时→单屏刷，SD/文件失败→页码 demo，都不挡测试。

#include <ESP32epdx.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "src/EinkPanel.h"
#include "src/DualEink.h"
#include "src/TxtBook.h"
#include "src/CjkFont.h"
#include "src/PageStats.h"
#include "src/DemoPage.h"

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

// 深睡跨醒保留（RTC 内存）：屏上那一对页的文件位置/页号
RTC_DATA_ATTR long s_sleepFilePos = -1;
RTC_DATA_ATTR uint32_t s_sleepPage = 0;

static const uint32_t FULL_EVERY = 5;        // 每 5 次快刷插 1 次全刷
static const uint32_t PAGE_INTERVAL_MS = 5000;
static const uint32_t FAST_TARGET_MS = 1800; // 验收线：整页 ≤1.8s

EinkPanel left(PIN_L_CS, PIN_L_DC, PIN_L_RST, PIN_L_BUSY, EINK_SCK, EINK_MOSI);
EinkPanel right(PIN_R_CS, PIN_R_DC, PIN_R_RST, PIN_R_BUSY,
                EINK_SCK, EINK_MOSI, R_MIRRORED != 0);
DualEink dual(left, right);
TxtBook txt;
PageStats stats;
Preferences prefs;              // 字体/字号选择存 NVS（断电不丢）

static uint8_t g_size = TXT_SIZE_PX;   // 当前字号档（16/24/32）
static int g_fam = 0;                  // 当前家族（0=classic）

static void printFontState() {
  Serial.printf("[font] family=%s size=%u  (serial: f=next font, s=next size)\n",
                CJK.familyName(g_fam), g_size);
}

// 双缓冲 × 两屏：shown = 屏上当前帧，draw = 正在绘制的下一帧
uint8_t* fbShownL = nullptr;
uint8_t* fbDrawL = nullptr;
uint8_t* fbShownR = nullptr;
uint8_t* fbDrawR = nullptr;
bool dualOk = false;   // 两屏都就绪；false = 单屏退化
bool txtOk = false;    // TXT 可用；false = 退页码 demo
uint32_t pageCount = 0;    // 刷新次数（管节奏 + 统计）
uint32_t demoPage = 0;     // demo 页号（TXT 不可用时用）

static uint8_t* allocFrame() {
  uint8_t* p = (uint8_t*)ps_malloc(EinkPanel::FULL_BYTES);
  if (!p) p = (uint8_t*)malloc(EinkPanel::FULL_BYTES);
  return p;
}

// 本次刷新要显示的左右两页内容：优先 TXT，不可用则退回页码 demo
static void preparePages(bool fullMode) {
  if (txtOk && !txt.renderPair(fbDrawL, fbDrawR)) {
    Serial.println("WARN: txt render failed - fallback to demo pages");
    txtOk = false;
  }
  if (!txtOk) {
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
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== dual e-ink book demo ===");

  // 最先释放深睡 hold：EXT1 唤醒/外部 RST 后 hold 残留在 RTC 域，不放开会把
  // SPI 脚锁死在睡眠电平 → 屏 SPI 废掉，恢复渲染/翻页全挂（无 hold 状态下
  // 调用是无害 no-op，上电首跑也安全）
  for (size_t i = 0; i < sizeof(SLEEP_HOLD_PINS) / sizeof(SLEEP_HOLD_PINS[0]); i++)
    gpio_hold_dis((gpio_num_t)SLEEP_HOLD_PINS[i]);
  gpio_deep_sleep_hold_dis();

  pinMode(PIN_HALL, INPUT);   // DRV5032 推挽输出，不需内部上拉
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
  Paint_NewImage(fbShownL, EinkPanel::WIDTH, EinkPanel::HEIGHT, ROTATE_0, WHITE);

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

  // 中英混排测试文件优先，缺了退回纯英文（P3 回归）；根目录旧布局兜底
  txtOk = txt.open("/books/test_cn.txt") || txt.open("/books/test.txt") ||
          txt.open("/test_cn.txt") || txt.open("/test.txt");
  if (!txtOk) Serial.println("WARN: no txt on SD - demo pages only");

  // 恢复用户上次的字体/字号（NVS），首次默认 classic + TXT_SIZE_PX
  if (txtOk) {
    g_size = prefs.getUChar("size", TXT_SIZE_PX);
    String fam = prefs.getString("font", "classic");
    txt.setTier(g_size);
    txt.setFamily(fam.c_str());
    g_fam = CJK.currentFamily();
    printFontState();
  }

  // 第 0/1 页：全刷（shown 还是白的）→ 之后 shown 永远 = 屏上真实内容。
  // EXT1 深睡唤醒例外：屏上图像没丢（VCI 常供），把存档对页重画进 fbDraw 再
  // memcpy 到 fbShown —— shown = 屏幕真实内容，**不推送面板** → 恢复零闪烁，
  // 下一翻页的 diff/DTM1 也正确。屏从头到尾一直在显示，"开盖即恢复"
  bool resumed = false;
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("[wake] EXT1 (lid opened) - resuming book");
    if (txtOk && s_sleepFilePos >= 0 && txt.restorePair(s_sleepFilePos, s_sleepPage)) {
      preparePages(true);
      memcpy(fbShownL, fbDrawL, EinkPanel::FULL_BYTES);
      memcpy(fbShownR, fbDrawR, EinkPanel::FULL_BYTES);
      resumed = true;
      Serial.printf("[wake] page %lu re-rendered into RAM, panel untouched\n",
                    (unsigned long)s_sleepPage);
    }
  }
  if (!resumed) runRefresh(true);
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
  s_sleepFilePos = txtOk ? txt.pairFilePos() : -1;
  s_sleepPage    = txtOk ? txt.pairPageNo() : 0;
  Serial.printf("[%lu ms] lid closed -> deep sleep (pos=%ld page=%lu)\n",
                (unsigned long)millis(), s_sleepFilePos, (unsigned long)s_sleepPage);
  if (Serial) Serial.flush();

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
  if (txtOk && txt.restartPair()) runRefresh(false);
}

// 串口命令：f = 切字体家族（SD 包自动发现），s = 切字号 16/24/32，h = 状态
// （P6 4 键落地后绑同一套 setFamily/setTier）
static void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'f' && CJK.familyCount() > 1) {
      g_fam = (g_fam + 1) % CJK.familyCount();
      txt.setFamily(CJK.familyName(g_fam));
      g_fam = CJK.currentFamily();              // 包装载失败会自动回 classic
      prefs.putString("font", CJK.familyName(g_fam));
      printFontState();
      previewFontChange();
    } else if (c == 's') {
      g_size = (g_size == 16) ? 24 : (g_size == 24 ? 32 : 16);
      txt.setTier(g_size);
      g_fam = CJK.currentFamily();
      prefs.putUChar("size", g_size);
      printFontState();
      previewFontChange();
    } else if (c == 'h') {
      printFontState();
    }
  }
}

void loop() {
  handleSerial();

  // 分片等待下一次翻页，每 20ms 查一次霍尔：合盖 300ms 防抖 → 深睡（不返回）。
  // 原来一整段 delay(5s) 期间是聋的，合盖要等最多 5s 才睡
  uint32_t waitStart = millis();
  while (!hallClosing() && millis() - waitStart < PAGE_INTERVAL_MS) delay(20);
  if (hallClosing()) enterDeepSleep();

  pageCount++;
  runRefresh(pageCount % (FULL_EVERY + 1) == 0);
  if (pageCount % 100 == 0) stats.printSummary(pageCount, FAST_TARGET_MS);
}
