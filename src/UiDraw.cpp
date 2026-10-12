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

// ---- 美化辅助 ----

// 粗分隔线（用于标题下、页脚上等主分隔）
void drawThickRule(int x1, int y, int x2, int thick) {
  for (int t = 0; t < thick; t++)
    Paint_DrawLine(x1, y + t, x2, y + t, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
}

// 细分隔线（1px）
void drawThinRule(int x1, int y, int x2) {
  Paint_DrawLine(x1, y, x2, y, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
}

// 选中项左侧实心三角（指向右方，像播放键）
void drawSelectArrow(int x, int y) {
  // 三角形：高 12px，宽 8px，逐行画水平线
  for (int row = 0; row < 12; row++) {
    int w = (row < 6) ? (row + 1) * 8 / 12 : (11 - row) * 8 / 12;
    if (w < 1) w = 1;
    int cy = y + row;
    Paint_DrawLine(x, cy, x + w, cy, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
  }
}

// 选中项粗下划线（3px）
void drawSelectBar(int x1, int y, int x2) {
  drawThickRule(x1, y, x2, 3);
}

// 分节标题：小实心方块 + 标题文字（size 应为 16 或 32）
void drawSectionHead(int x, int y, const char* s, int size) {
  int bulletSz = (size >= 24) ? 8 : 6;
  int bulletY = y + (size - bulletSz) / 2;
  Paint_DrawRectangle(x, bulletY, x + bulletSz - 1, bulletY + bulletSz - 1,
                      BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
  int textX = x + bulletSz + 6;
  int i = 0, len = (int)strlen(s);
  while (i < len) {
    int u;
    uint32_t cp = utf8At(s + i, len - i, &u);
    if (textX + size > EinkPanel::PAGE_W - 24) break;
    CJK.drawGlyph(textX, y, cp, size);
    textX += size;
    i += u;
  }
}

// 小实心圆点（列表项前缀装饰）
void drawBullet(int x, int y) {
  Paint_DrawCircle(x, y + 7, 3, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
}

// 状态指示点：实心=开，空心=关
void drawStatusDot(int x, int y, bool on) {
  Paint_DrawCircle(x, y + 7, 4, BLACK,
                   on ? DRAW_FILL_FULL : DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
}

// 测量混排字符串宽度（ASCII 11px + CJK 16px）
int measureUtf8(const char* s) {
  int w = 0, i = 0, len = (int)strlen(s);
  while (i < len) {
    int u;
    uint32_t cp = utf8At(s + i, len - i, &u);
    w += (cp < 0x80) ? 11 : 16;
    i += u;
  }
  return w;
}

// 居中混排行
void drawCenteredUtf8(int y, const char* s, int maxX) {
  int w = measureUtf8(s);
  int x = (maxX + 24 - w) / 2;  // 24 = 左边距
  if (x < 24) x = 24;
  drawUtf8(x, y, s, maxX);
}
