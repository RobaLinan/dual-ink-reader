#include "Keys.h"

#include "driver/gpio.h"
#include "esp_timer.h"

// === 参数（key_test 台架 2026-10-08 实测，见 src/Keys.h 头注）===
static const uint32_t DEBOUNCE_US = 20000;    // 20ms 去抖
static const uint32_t LONG_US = 250000;       // 250ms 长按（WIRING §3.4 台架定标：
                                              // 实测按住 115–800ms，阈值取 ~250ms）
static const uint32_t REPEAT_US = 300000;     // 长按后每 300ms 连发
static const int EDGE_N = 16;                 // 边沿环长（2 的幂）
static const int EV_N = 8;                    // 事件队列长

// WIRING §3.4：Btn_Prev=GPIO1 Btn_Next=GPIO2 Btn_Menu=GPIO9 Btn_Font=GPIO10
const uint8_t Keys::s_pins[Keys::COUNT] = {1, 2, 9, 10};

// 长按连发只给翻页键：菜单内 Font 长按不重复，Menu 长按无意义
static const bool kRepeatable[Keys::COUNT] = {true, true, false, false};

portMUX_TYPE Keys::s_mux = portMUX_INITIALIZER_UNLOCKED;
Keys::Edge Keys::s_edges[EDGE_N];
volatile uint8_t Keys::s_eHead = 0;
volatile uint8_t Keys::s_eTail = 0;
volatile uint32_t Keys::s_edgeOf = 0;

Keys keys;   // 全局唯一实例（eink.ino 组装时用）

// ISR 只记 (时刻, 脚, 电平)，判定全在 poll：满队列丢最旧 —— 保"最新电平
// 状态"正确（丢最新会把释放沿吞掉 → 状态卡在按下）
void IRAM_ATTR Keys::edgeIsr(void* arg) {
  uint8_t id = (uint8_t)(uintptr_t)arg;
  Edge e;
  e.us = (uint32_t)esp_timer_get_time();
  e.id = id;
  e.level = (uint8_t)gpio_get_level((gpio_num_t)s_pins[id]);

  portENTER_CRITICAL_ISR(&s_mux);
  uint8_t next = (uint8_t)((s_eHead + 1) & (EDGE_N - 1));
  if (next == s_eTail) {
    s_eTail = (uint8_t)((s_eTail + 1) & (EDGE_N - 1));   // 溢出：丢最旧
    s_edgeOf++;
  }
  s_edges[s_eHead] = e;
  s_eHead = next;
  portEXIT_CRITICAL_ISR(&s_mux);
}

void Keys::begin() {
  uint32_t now = (uint32_t)esp_timer_get_time();
  for (uint8_t i = 0; i < COUNT; i++) {
    pinMode(s_pins[i], INPUT);           // 板上有 10k↑，不用内部上拉
    // 起始电平**不**当作状态：醒后/开机时被按住的键要"忽略这一次"，
    // 于是 down/armed 全清零 —— 之后只有真实按沿才武装事件（DESIGN §4 P6）
    m_down[i] = false;
    m_armed[i] = false;
    m_longFired[i] = false;
    m_pressUs[i] = now;
    m_lastAccept[i] = now;               // 上电 20ms 内的边沿当毛刺
    attachInterruptArg(digitalPinToInterrupt(s_pins[i]), edgeIsr,
                       (void*)(uintptr_t)i, CHANGE);
  }
  m_evHead = m_evTail = 0;
}

void Keys::pushEv(uint8_t id, uint8_t ev, bool longFired) {
  uint8_t next = (uint8_t)((m_evHead + 1) % EV_N);
  if (next == m_evTail) {                // 满：丢最旧（保留最新意图）
    m_evTail = (uint8_t)((m_evTail + 1) % EV_N);
    m_evOf++;
  }
  m_events[m_evHead].id = id;
  m_events[m_evHead].ev = ev;
  m_events[m_evHead].longFired = longFired;
  m_evHead = next;
}

bool Keys::take(Event& out) {
  if (m_evTail == m_evHead) return false;
  out = m_events[m_evTail];
  m_evTail = (uint8_t)((m_evTail + 1) % EV_N);
  return true;
}

void Keys::poll() {
  uint32_t now = (uint32_t)esp_timer_get_time();

  // ① 攒下的边沿 → 去抖 → 按下/松开事件
  for (;;) {
    Edge e;
    portENTER_CRITICAL(&s_mux);
    if (s_eTail == s_eHead) { portEXIT_CRITICAL(&s_mux); break; }
    e = s_edges[s_eTail];
    s_eTail = (uint8_t)((s_eTail + 1) & (EDGE_N - 1));
    portEXIT_CRITICAL(&s_mux);

    if ((uint32_t)(e.us - m_lastAccept[e.id]) < DEBOUNCE_US) { m_bounce++; continue; }
    m_lastAccept[e.id] = e.us;

    if (e.level == 0) {                         // 按下沿
      if (m_down[e.id]) continue;
      m_down[e.id] = true;
      m_armed[e.id] = true;                     // 武装：释放时会发 RELEASE
      m_longFired[e.id] = false;
      m_pressUs[e.id] = e.us;
      m_nextRepeatUs[e.id] = e.us + LONG_US + REPEAT_US;
      pushEv(e.id, PRESS);
    } else {                                    // 松开沿
      if (!m_down[e.id]) continue;              // 开机前就按住的键：吞掉释放
      m_down[e.id] = false;
      bool lf = m_longFired[e.id];
      // 整段按住都落在刷新窗口（poll 没跑，长按没来得及判）→ 松开时补判
      if (m_armed[e.id] && !lf && (uint32_t)(e.us - m_pressUs[e.id]) >= LONG_US) {
        m_longFired[e.id] = lf = true;
        pushEv(e.id, LONG, true);
      }
      if (m_armed[e.id]) pushEv(e.id, RELEASE, lf);
      m_armed[e.id] = false;
    }
  }

  // ② 长按到点 / 长按后连发（poll 不跑期间暂停，恢复后按 now 顺延一拍，
  //    不做欠账补发 —— 避免刷新结束瞬间连打一串翻页）
  for (uint8_t i = 0; i < COUNT; i++) {
    if (!m_down[i]) { m_highSince[i] = 0; continue; }

    // 电平兜底（见头注）：m_down 与引脚实际电平背离满一个去抖窗 → 补发释放
    if (gpio_get_level((gpio_num_t)s_pins[i]) != 0) {
      if (!m_highSince[i]) { m_highSince[i] = now; continue; }
      if ((uint32_t)(now - m_highSince[i]) < DEBOUNCE_US) continue;
      m_down[i] = false;
      m_highSince[i] = 0;
      bool lf = m_longFired[i];
      // 与松开沿同款的补判：整段按住落在刷新窗口里、LONG 没来得及发
      if (m_armed[i] && !lf && (uint32_t)(now - m_pressUs[i]) >= LONG_US) {
        m_longFired[i] = lf = true;
        pushEv(i, LONG, true);
      }
      if (m_armed[i]) pushEv(i, RELEASE, lf);
      m_armed[i] = false;
      continue;
    }
    m_highSince[i] = 0;

    if (!m_longFired[i]) {
      if ((int32_t)(now - m_pressUs[i]) >= (int32_t)LONG_US) {
        m_longFired[i] = true;
        m_nextRepeatUs[i] = now + REPEAT_US;
        pushEv(i, LONG);
      }
    } else if (kRepeatable[i] && (int32_t)(now - m_nextRepeatUs[i]) >= 0) {
      m_nextRepeatUs[i] = now + REPEAT_US;
      pushEv(i, REPEAT);
    }
  }
}

const char* Keys::name(uint8_t id) {
  switch (id) {
    case PREV:  return "Prev";
    case NEXT:  return "Next";
    case MENU:  return "Menu";
    case FONT:  return "Font";
    default:    return "?";
  }
}

const char* Keys::evName(uint8_t ev) {
  switch (ev) {
    case PRESS:   return "press";
    case LONG:    return "long";
    case REPEAT:  return "repeat";
    case RELEASE: return "release";
    default:      return "none";
  }
}
