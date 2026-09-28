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

  // SD 写失败自救：end() + begin() 重挂（卡走 CMD0 重新初始化，驱动状态全清）。
  // 调用纪律：**先把所有 SD 文件句柄关掉**（Book::close / EpubZip::close / …），
  // 重挂后旧句柄全部失效。挂载失败不缓存，返回 false 表示这轮没救（调用方
  // 自己决定要不要再试——每次失败可能连环 5s 写超时，别无脑重来）
  //
  // ⚠️ 返回 true 的判据只到"挂载成功 + 能开根目录"，**不验写**。09-28 实测：
  //    recover 打了 OK(40ms)，紧接着 SD_MMC.remove() 仍 0x107（DESIGN 坑 18 末段）
  //    —— 卡处于"能初始化、不能写"状态。别把 OK 当"SD 可用"，真要写还得查返回值。
  static bool recover();
};

#endif  // STORAGE_H
