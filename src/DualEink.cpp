#include "DualEink.h"

DualEink::DualEink(EinkPanel& left, EinkPanel& right)
    : m_l(left), m_r(right) {}

bool DualEink::begin() {
  // 两屏都跑一遍 begin()，即使左屏失败也不 short-circuit —— 要能从串口
  // 分别看到左右的接线结果
  bool okL = m_l.begin();
  bool okR = m_r.begin();
  return okL && okR;
}

bool DualEink::present(const uint8_t* prevL, const uint8_t* nextL,
                       const uint8_t* prevR, const uint8_t* nextR,
                       EinkPanel::Speed s, uint32_t timeoutMs) {
  if (!waitBoth(timeoutMs)) return false;  // 上一轮还有屏没刷完

  // 屏L：配置 + 灌帧 + 触发。trigger() 内含 1ms 延时（数据手册 0x12 后
  // ≥200µs），保证等下 waitBoth 时左屏 BUSY 已经拉起来
  m_l.setSpeed(s);
  m_l.loadFrame(prevL, nextL);
  m_l.trigger();

  // 趁左屏 busy 给右屏灌数据（这就是重叠：右屏的 SPI 时间藏在左屏的刷新里）
  m_r.setSpeed(s);
  m_r.loadFrame(prevR, nextR);
  m_r.trigger();

  return waitBoth(timeoutMs);
}

bool DualEink::waitBoth(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (!m_l.idle() || !m_r.idle()) {  // 高电平 = 空闲
    if (millis() - start > timeoutMs) return false;
    delay(1);  // 喂狗 + 让出调度
  }
  return true;
}
