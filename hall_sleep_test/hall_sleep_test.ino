// 霍尔深睡/唤醒台架测试（P3 最后一项的硬件验证，见 WIRING §3.5 J6 / DESIGN §5）
//
// 接线：DRV5032 模块 VCC→3V3、GND→GND、OUT→GPIO4（推挽输出，无需上拉）
//
// 行为：
//   合盖（磁铁靠近 → DRV5032 输出 LOW）稳定 300ms → 深睡（EXT1 挂起）
//   开盖（磁场消失 → 输出翻 HIGH）→ EXT1 唤醒，RGB LED 亮蓝，串口打印恢复用时
//
// 观察点：
//   1. 心跳每秒一行 `awake hall=X` —— 先拿磁铁靠近/离开验极性（应 1→0→1）
//   2. 合盖 → `lid closed -> deep sleep` → 红灯一闪 → USB 掉线（深睡属正常）
//   3. 开盖 → RGB LED 几乎瞬间变蓝（这就是"开盖 2s 恢复"的体感判定），
//      串口重新枚举后打印 `[ N ms] wake=EXT1` —— N 含 USB 重枚举等待，
//      只作参考；LED 亮的时刻才是真实唤醒时刻
//   4. 按 RST → `wake=POWERON/RESET`，区分复位和唤醒
//
// 失败模式：
//   - 心跳电平乱跳/合盖不睡 = 引脚浮空或接线错（推挽输出不应浮空）
//   - 极性反了（磁铁靠近反而 hall=1）→ 改 HALL_CLOSED_LEVEL 为 1
//   - 锁存型模块（误买）→ 磁场撤走输出不翻转，永远叫不醒，换开关型（DESIGN §1 注）

#include "esp_sleep.h"

static const gpio_num_t HALL_PIN = GPIO_NUM_4;      // WIRING §1：HALL_IN
static const uint64_t HALL_MASK = 1ULL << HALL_PIN; // GPIO4 ∈ S3 RTC IO(0-21)，EXT1 合法

#ifndef HALL_CLOSED_LEVEL
#define HALL_CLOSED_LEVEL 0   // 合盖电平：DRV5032 磁场有效=LOW；反相模块改 1
#endif

// 唤醒条件 = 离开合盖电平：合盖 LOW 睡 → 开盖变高醒
#if HALL_CLOSED_LEVEL == 0
static const esp_sleep_ext1_wakeup_mode_t WAKE_MODE = ESP_EXT1_WAKEUP_ANY_HIGH;
#else
static const esp_sleep_ext1_wakeup_mode_t WAKE_MODE = ESP_EXT1_WAKEUP_ALL_LOW;
#endif

static const int RGB_PIN = 48;   // DevKitC-1 板载 WS2812；本测试不驱动屏，脚空闲可用

RTC_DATA_ATTR uint32_t s_bootCount = 0;   // 跨深睡保留：累计唤醒/启动次数

static const char* causeStr(esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_EXT1:     return "EXT1 (lid opened)";
    case ESP_SLEEP_WAKEUP_EXT0:     return "EXT0";
    case ESP_SLEEP_WAKEUP_TIMER:    return "TIMER";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "POWERON/RESET";
    default:                        return "?";
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(HALL_PIN, INPUT);     // DRV5032 推挽；若换裸开漏管/无上拉模块改 INPUT_PULLUP
  pinMode(RGB_PIN, OUTPUT);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  s_bootCount++;

  neopixelWrite(RGB_PIN, 0, 0, cause == ESP_SLEEP_WAKEUP_EXT1 ? 48 : 0);  // EXT1 醒=蓝

  // 深睡唤醒 = USB 重新枚举，枚举完成前的打印会丢：等串口就绪（≤4s）。
  // 打进 millis = 上电到此刻的毫秒数（含枚举等待，只作参考）
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);
  Serial.printf("\n[%lu ms] wake=%s | hall=%d | boot #%lu\n",
                (unsigned long)millis(), causeStr(cause),
                digitalRead(HALL_PIN), (unsigned long)s_bootCount);
  Serial.println("ready: magnet near -> hall=0 -> sleep; away -> EXT1 wake");
}

static uint32_t s_lastBeat = 0;
static uint32_t s_closeSince = 0;   // 0 = 当前不在合盖电平

void loop() {
  int level = digitalRead(HALL_PIN);
  uint32_t now = millis();

  if (level == HALL_CLOSED_LEVEL) {
    if (!s_closeSince) {
      s_closeSince = now;
    } else if (now - s_closeSince >= 300) {          // 300ms 防抖
      Serial.printf("[%lu ms] lid closed -> deep sleep\n", (unsigned long)now);
      if (Serial) Serial.flush();
      neopixelWrite(RGB_PIN, 48, 0, 0);              // 睡前红灯一闪
      delay(120);
      neopixelWrite(RGB_PIN, 0, 0, 0);
      esp_sleep_enable_ext1_wakeup(HALL_MASK, WAKE_MODE);
      esp_deep_sleep_start();                        // 只会从 EXT1 醒来
    }
  } else {
    s_closeSince = 0;
  }

  if (now - s_lastBeat >= 1000) {
    s_lastBeat = now;
    Serial.printf("[%lu ms] awake hall=%d%s\n", (unsigned long)now, level,
                  s_closeSince ? "  (close pending...)" : "");
  }
  delay(20);
}
