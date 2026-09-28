#ifndef EPUB_IMG_H
#define EPUB_IMG_H

#include <Arduino.h>

// EPUB 插图解码（DESIGN §5 P4 插图）：文件 → 当前 Paint 画布上的 1bit 抖动位图。
//   probe = 读文件头探原始尺寸（不解码）：JPEG 扫 SOF 段 / PNG 读 IHDR
//   draw  = 解码 + 等比缩放（放大不超限）+ 4×4 Bayer 有序抖动 + Paint_SetPixel 落点
// 格式：JPEG 走 vendored ChaN TJpgDec（块回调式，RGB565 输出，峰值 RAM = 3.5KB
// 工作区，与图片分辨率无关，坑 19：S3 ROM 的 esp_rom_tjpgd 实测 ABI 不符已弃）；
// PNG 走 PNGdec 库（~/Arduino/libraries，inflate 用块回调）。
// GIF/WebP/SVG/渐进 JPEG 不支持：probe 判不出 → 返回 false，排版层跳过该图。
// 调用约定：draw 前必须已 Paint_SelectImage 目标帧缓冲；白底（只点黑，帧先刷白）。

namespace EpubImg {
  // false = 不是 JPEG/PNG 或文件坏/打不开
  bool probe(const char* path, int* w, int* h);
  // (x,y) 左上、maxW×maxH 界限内等比缩放居中绘制。false = 解码失败（画布不动）
  bool draw(const char* path, int x, int y, int maxW, int maxH);
}

#endif  // EPUB_IMG_H
