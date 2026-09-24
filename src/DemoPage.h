#ifndef DEMO_PAGE_H
#define DEMO_PAGE_H

#include <Arduino.h>

// 兜底页码页：SD/TXT 不可用时的演示内容（也能目视确认差分刷新没有整屏闪烁：
// 实心块随页码移动、左右位置不同）
void drawDemoPage(uint8_t* buf, uint32_t page, const char* side, bool fullMode);

#endif  // DEMO_PAGE_H
