#ifndef DUAL_EINK_H
#define DUAL_EINK_H

#include "EinkPanel.h"

// 双屏重叠刷新（P2）。API 形状对照 Diptyx 固件的
// InitNormalBoth / DisplayPictureBoth / WaitUntilIdleBoth：
//
//   present(L_prev, L_next, R_prev, R_next, speed):
//     1. 等两屏 idle（BUSY 期间控制器拒收新命令）
//     2. 屏L：setSpeed + loadFrame(DTM1/DTM2) → DRF，不等待
//     3. 趁屏L busy 的 ~1.2s 给屏R 灌数据 → DRF
//        （两屏独立 CS，共享 SCK/MOSI；SPI 按"选片→传输→释放"严格串行，
//         灌右屏数据只占总线 ~60ms，不打断屏L 自己的刷新）
//     4. 等两屏都 idle
//
// 单屏串行 ≈ 2×1.3s → 重叠后 ≈ max(单屏) + ~60ms，验收线 ≤1.8s。
// 代价：两屏升压同时抽电（峰值 300-400mA，见 DESIGN §2 电流尖峰预案）。
class DualEink {
public:
  DualEink(EinkPanel& left, EinkPanel& right);

  // 两屏都做 SPI init + 硬复位 + PON。false = 至少一屏 BUSY 超时（查接线）
  bool begin();

  // 左右异图差分刷新。false = BUSY 超时
  // （超时后可用 idle() 分别查左右哪屏没回来）
  bool present(const uint8_t* prevL, const uint8_t* nextL,
               const uint8_t* prevR, const uint8_t* nextR,
               EinkPanel::Speed s, uint32_t timeoutMs = 8000);

  // 等两屏都空闲。true = 正常，false = 超时
  bool waitBoth(uint32_t timeoutMs);

  EinkPanel& left() { return m_l; }
  EinkPanel& right() { return m_r; }

private:
  EinkPanel& m_l;
  EinkPanel& m_r;
};

#endif  // DUAL_EINK_H
