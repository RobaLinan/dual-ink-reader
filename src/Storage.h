#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>

// P3 storage 层雏形：SDMMC 1-bit @20MHz（DESIGN §1.1 正式引脚 + §5.7 定案）
//   CLK 38 / CMD 39 / D0 40 —— 与屏 SPI(12/11)、按键(1/2/9/10)、霍尔(4) 全部错开
// 8.21 测速（写 2MB/1417ms ≈1.41MB/s）用的临时脚 12/11/10 与屏冲突，已弃用。
//
// 挂载失败不 fatal：上层降级（demo 退回页码页），P3 正式版再决定错误页策略。
class Storage {
public:
  static bool begin();     // setPins + 挂载（失败指数退避重试 5 次，防冷上电 0x107）
  static bool mounted();
};

#endif  // STORAGE_H
