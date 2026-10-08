// 板载 4 键台架测试（P6 第一项硬件验证，见 WIRING §3.4 / DESIGN §4 P6）
//
// 接线：无 —— 按键在主板上（Btn_Prev/Btn_Next/Btn_Menu/Btn_Font），
//       10k 上拉 R6–R9 在主板侧，按下把信号拉到 GND（低有效）。
//       本 sketch 不驱动屏/SD，只读这 4 个脚（GPIO1/2/9/10，S3 RTC IO，
//       与霍尔同为 RTC IO；唤醒定案 2026-10-08：只有霍尔走 EXT1，按键不唤醒，
//       此处只验电气 + 去抖 + 计数）。
//
// 行为：
//   上电打印 4 脚空闲电平（应全 =1）→ 每秒心跳一行电平+计数 →
//   按下/松开各打印一行（含按住时长、累计次数）→ 按住期间 RGB LED 亮。
//
// 观察点：
//   1. 启动行 `idle P=1 N=1 M=1 F=1` —— 任一为 0 = 焊桥或按键另一端没接 GND
//      （原理图 2026-09-29 修过"BTN_* 误标在 GND 脚"，这一步就验它）
//   2. 逐键连按 10 次：`press Prev #1..#10` 计数连续跳号 = 漏键（虚接/去抖过长）
//   3. `bounce` 计数 >0 = 去抖窗口（20ms）内被滤掉的毛刺；新键偶发正常，
//      高频出现 = 触点质量差或走线串扰 → 换键/查布线
//   4. 心跳里出现 0 而你没按 = 间歇性虚接或对 GND 短路
//   5. 同时按两键 → `combo Prev+Next` —— 查按键间连锡（BOM/WIRING 验收项）
//   6. 按住不放看长按：`release Prev held 5000ms` —— 长按阈值给后续 4 键逻辑定标
//   7. 串口命令：`r` 清计数 / `?` 立即打印状态 / `h` 帮助
//
// 失败模式：
//   - 启动即有 0 → 按 §1 查焊桥，别往下测
//   - 心跳乱跳 → 上拉失效（R6–R9 虚焊）或 3V3 没供上
//   - 计数比实际按压少 → 去抖太严或触点抖动超 20ms，先换键复测再改 DEBOUNCE_MS

static const int RGB_PIN = 48;   // DevKitC-1 板载 WS2812；不驱动屏，脚空闲可用

#ifndef DEBOUNCE_MS
#define DEBOUNCE_MS 20   // 稳定电平保持该时长才认边沿（WIRING §3.4：低有效）
#endif

struct Key {
  const char* name;   // 短名（Prev/Next/Menu/Font）
  char tag;           // 心跳里的单字母（P/N/M/F）
  uint8_t pin;
  int stable;         // 稳定电平：1=释放 0=按下；初值取上电读数
  int cand;           // 候选电平（等 DEBOUNCE_MS 确认）
  uint32_t candAt;    // 候选电平出现时刻
  uint32_t pressAt;   // 本次按下时刻（算 held）
  uint32_t lastPressAt; // 上次按下时刻（算连按间隔）
  uint32_t count;     // 累计按下次数
  uint32_t bounce;    // 被去抖滤掉的毛刺次数
};

// WIRING §3.4：Btn_Prev=GPIO1 Btn_Next=GPIO2 Btn_Menu=GPIO9 Btn_Font=GPIO10
static Key KEYS[] = {
  {"Prev", 'P', 1, 1, 1, 0, 0, 0, 0, 0},
  {"Next", 'N', 2, 1, 1, 0, 0, 0, 0, 0},
  {"Menu", 'M', 9, 1, 1, 0, 0, 0, 0, 0},
  {"Font", 'F', 10, 1, 1, 0, 0, 0, 0, 0},
};
static const int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);

static int heldCount() {
  int n = 0;
  for (int i = 0; i < NKEYS; i++) n += (KEYS[i].stable == 0);
  return n;
}

static void printStatus(uint32_t now) {
  Serial.printf("[%lu ms] lvl", (unsigned long)now);
  for (int i = 0; i < NKEYS; i++) Serial.printf(" %c=%d", KEYS[i].tag, KEYS[i].stable);
  Serial.print(" | cnt");
  for (int i = 0; i < NKEYS; i++) Serial.printf(" %c=%lu", KEYS[i].tag,
                                                (unsigned long)KEYS[i].count);
  Serial.print(" | bnc");
  for (int i = 0; i < NKEYS; i++) Serial.printf(" %c=%lu", KEYS[i].tag,
                                                (unsigned long)KEYS[i].bounce);
  Serial.println();
}

static void printHelp() {
  Serial.println("cmd: r=reset counters  ?=status now  h=help");
}

void setup() {
  Serial.begin(115200);
  pinMode(RGB_PIN, OUTPUT);
  neopixelWrite(RGB_PIN, 0, 0, 0);

  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);   // 深睡/复位后等 USB 枚举

  // 脚位初始化：读上电电平当稳定态（不强制拉高，验的就是板上 10k 上拉）
  Serial.printf("\n[%lu ms] key_test: debounce=%dms, active=LOW\n",
                (unsigned long)millis(), DEBOUNCE_MS);
  for (int i = 0; i < NKEYS; i++) {
    Key& k = KEYS[i];
    pinMode(k.pin, INPUT);            // 板上有 10k↑，不用内部上拉
    int lvl = digitalRead(k.pin);
    k.stable = k.cand = lvl;
    k.candAt = millis();
    Serial.printf("  %s = GPIO%-2d idle=%d%s\n", k.name, k.pin, lvl,
                  lvl == 0 ? "   <- LOW at idle: solder bridge / no GND return" : "");
  }
  Serial.println("ready: press each key 10x; combo test = hold one, press another");
  printHelp();
  printStatus(millis());
}

static uint32_t s_lastBeat = 0;

void loop() {
  uint32_t now = millis();

  for (int i = 0; i < NKEYS; i++) {
    Key& k = KEYS[i];
    int raw = digitalRead(k.pin);

    if (raw != k.cand) {
      // 候选电平还没熬满去抖窗口就翻回去 = 触点抖动/毛刺，被滤掉，计一次
      if (k.cand != k.stable && now - k.candAt < DEBOUNCE_MS) k.bounce++;
      k.cand = raw;
      k.candAt = now;
    }

    if (k.cand != k.stable && now - k.candAt >= DEBOUNCE_MS) {
      int was = k.stable;
      k.stable = k.cand;

      if (was == 1 && k.stable == 0) {              // 按下沿
        k.pressAt = now;
        k.count++;
        uint32_t gap = k.lastPressAt ? now - k.lastPressAt : 0;
        k.lastPressAt = now;
        Serial.printf("[%lu ms] press %s #%lu (gap %lums)\n", (unsigned long)now,
                      k.name, (unsigned long)k.count, (unsigned long)gap);
        neopixelWrite(RGB_PIN, 0, 0, 48);           // 按住 = 亮
        if (heldCount() >= 2) {
          Serial.print("         combo:");
          bool first = true;
          for (int j = 0; j < NKEYS; j++)
            if (KEYS[j].stable == 0) {
              Serial.printf("%s%s", first ? " " : "+", KEYS[j].name);
              first = false;
            }
          Serial.println();                          // 多键同按 → 查连锡
        }
      } else if (was == 0 && k.stable == 1) {       // 松开沿
        Serial.printf("[%lu ms] release %s held %lums\n", (unsigned long)now, k.name,
                      (unsigned long)(now - k.pressAt));
        if (heldCount() == 0) neopixelWrite(RGB_PIN, 0, 0, 0);
      }
    }
  }

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r') {
      for (int i = 0; i < NKEYS; i++) KEYS[i].count = KEYS[i].bounce = 0;
      Serial.println("counters reset");
    } else if (c == '?') {
      printStatus(now);
    } else if (c == 'h') {
      printHelp();
    }
  }

  if (now - s_lastBeat >= 1000) {   // 心跳：无按键时也能看出电平是否乱跳
    s_lastBeat = now;
    printStatus(now);
  }
  delay(5);
}
