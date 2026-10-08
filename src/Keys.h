#ifndef KEYS_H
#define KEYS_H

#include <Arduino.h>

// P6 板载 4 键（WIRING §3.4：Btn_Prev/Next/Menu/Font = GPIO1/2/9/10，
// 低有效，板载 10k↑→3V3；唤醒定案 2026-10-08：按键不参与 EXT1，只有霍尔唤醒）。
//
// 为什么是"ISR 边沿队列 + poll 判定"而不是轮询 digitalRead：
//   一次刷新阻塞 1.3s（快刷）/ 3.8s（全刷），期间 loop 不跑 —— 轮询会把
//   刷新窗口内按下又松开的键整段漏掉（用户翻页时最常按的就是刷新中）。
//   ISR 只做一件事：把 (脚, 电平, 时间戳) 丢进环形队列；判定全在 poll()
//   做，时间戳用边沿自己的时刻 → 刷新结束后照样能还原"按下过"。
//
// 判定参数（key_test 台架实测 2026-10-08：按键 4×10 次 bnc=0，按住 115–800ms）：
//   去抖 20ms：相邻边沿 <20ms 视作触点抖动丢弃（计入 bounce）
//   长按 600ms：到点发 LONG；到点前松开发 RELEASE(longFired=false)=短按
//   连发 300ms：仅 PREV/NEXT（长按翻页连发；菜单内 = 上下滚动）
//   醒后/开机时已被按住的键：不补发按下（释放时 down=false 直接吞掉）
//   —— 深睡中按过的键不产生幽灵事件（DESIGN §4 P6 进行中）
class Keys {
public:
  enum Id : uint8_t { PREV = 0, NEXT, MENU, FONT, COUNT };
  enum Ev : uint8_t { NONE = 0, PRESS, LONG, REPEAT, RELEASE };

  struct Event {
    uint8_t id;         // Id
    uint8_t ev;         // Ev
    bool longFired;     // 仅 RELEASE 有意义：松开前是否已触发过长按
  };

  void begin();                    // 配脚 + 挂边沿中断（setup 调一次）
  void poll();                     // 每 20ms 调：出抖动判定 + 长按/连发
  bool take(Event& out);           // 取一个事件；队列空 = false

  uint32_t bounce() const { return m_bounce; }   // 被去抖丢弃的边沿数（诊断用）
  uint32_t overflow() const { return s_edgeOf + m_evOf; }  // 边沿环/事件队列溢出

  static const char* name(uint8_t id);   // "Prev"/"Next"/"Menu"/"Font"
  static const char* evName(uint8_t ev); // "press"/"long"/"repeat"/"release"

private:
  struct Edge { uint32_t us; uint8_t id; uint8_t level; };

  static void IRAM_ATTR edgeIsr(void* arg);
  void pushEv(uint8_t id, uint8_t ev, bool longFired = false);

  static const uint8_t s_pins[COUNT];   // GPIO 1/2/9/10

  // ISR 侧（spinlock 保护）：边沿环形队列，满则丢最旧（保电平状态正确）
  static portMUX_TYPE s_mux;
  static Edge s_edges[16];
  static volatile uint8_t s_eHead;
  static volatile uint8_t s_eTail;
  static volatile uint32_t s_edgeOf;   // 边沿环溢出计数（ISR 侧累加）

  // poll 侧（单写者）
  uint32_t m_lastAccept[COUNT] = {0};   // 上一条被采纳的边沿时刻（去抖基准）
  uint32_t m_pressUs[COUNT] = {0};      // 本次按下的时刻
  uint32_t m_nextRepeatUs[COUNT] = {0}; // 下一次连发时刻
  bool m_down[COUNT] = {false};         // 稳定按下状态
  bool m_armed[COUNT] = {false};        // 本次按下是否由按沿武装（醒来时已按住=false）
  bool m_longFired[COUNT] = {false};    // 本次按下是否已发过 LONG
  uint32_t m_bounce = 0;
  uint32_t m_evOf = 0;                  // 事件队列溢出（poll 侧累加）

  Event m_events[8];                    // 待消费事件（poll 唯一写者）
  uint8_t m_evHead = 0;
  uint8_t m_evTail = 0;
};

extern Keys keys;

#endif  // KEYS_H
