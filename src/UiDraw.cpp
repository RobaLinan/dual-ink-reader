#include "UiDraw.h"

#include <ESP32epdx.h>

#include "EinkPanel.h"
#include "CjkFont.h"

// 实现自 SysMenu.cpp（搬出来时逐字未改，见 UiDraw.h 头注）

// UTF-8 解码（与 TxtBook::utf8At 同实现）：*used = 消耗字节数；非法 → '?' 吃 1 字节
uint32_t utf8At(const char* s, int len, int* used) {
  const uint8_t* p = (const uint8_t*)s;
  if (len <= 0) { *used = 0; return '?'; }
  uint8_t b = p[0];
  if (b < 0x80) { *used = 1; return b; }
  int extra; uint32_t cp;
  if ((b & 0xE0) == 0xC0)      { cp = b & 0x1F; extra = 1; }
  else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; }
  else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; extra = 3; }
  else                         { *used = 1; return '?'; }
  if (extra >= len) { *used = 1; return '?'; }
  for (int i = 1; i <= extra; i++) {
    if ((p[i] & 0xC0) != 0x80) { *used = i; return '?'; }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *used = extra + 1;
  return cp;
}

// 混排行：ASCII 走 Font16（步进 11），汉字走 Unifont 16（步进 16）——
// 与 TxtBook::drawHeader 同一手法；画到 maxX 截断（竖版页只有 480 宽）
void drawUtf8(int x, int y, const char* s, int maxX) {
  int len = (int)strlen(s), i = 0;
  while (i < len) {
    int u;
    uint32_t cp = utf8At(s + i, len - i, &u);
    int adv = cp < 0x80 ? 11 : 16;
    if (x + adv > maxX) break;
    if (cp < 0x80) Paint_DrawChar(x, y, (char)cp, &Font16, BLACK, WHITE);
    else CJK.drawGlyph(x, y, cp, 16);
    x += adv;
    i += u;
  }
}

// 标题大字：Unifont 最近邻放大，size 取 2 的幂（16/32）才不发虚
void drawCjkStr(int x, int y, const char* s, int size) {
  int len = (int)strlen(s), i = 0;
  while (i < len) {
    int u;
    uint32_t cp = utf8At(s + i, len - i, &u);
    if (x + size > EinkPanel::PAGE_W - 24) break;
    CJK.drawGlyph(x, y, cp, size);
    x += size;
    i += u;
  }
}

void drawBorder() {
  Paint_DrawRectangle(1, 1, EinkPanel::PAGE_W - 2, EinkPanel::PAGE_H - 2,
                      BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_2X2);
}
