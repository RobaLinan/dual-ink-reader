#ifndef UI_DRAW_H
#define UI_DRAW_H

#include <Arduino.h>

// 整页 UI（系统菜单 SysMenu / 选书主页 HomePage）共用的排版辅助。
// 原为 SysMenu.cpp 的私有函数，加主页时抽出来共用 —— 两页 UI 的排版口径
// 只留一份（坐标/步进/截断规则一改全改，不会菜单和主页长得不一样）：
//   utf8At     UTF-8 解码：*used = 消耗字节数；非法字节 → '?' 吃 1 字节
//              （与 TxtBook::utf8At 同实现）
//   drawUtf8   混排行：ASCII 走 Font16（步进 11）、汉字走 Unifont 16（步进 16），
//              画到 maxX 截断（竖版页只有 480 宽，见 EinkPanel::PAGE_W）
//   drawCjkStr 标题大字：Unifont 最近邻放大，size 取 2 的幂（16/32）才不发虚
//   drawBorder 2px 页框
//
// 前提：画布已 Paint_SelectImage 且 Paint_Clear(WHITE)（只写黑像素）；
// 字库未装载时由调用方（SysMenu::draw / HomePage::draw）先补 CJK.begin()。
uint32_t utf8At(const char* s, int len, int* used);
void drawUtf8(int x, int y, const char* s, int maxX);
void drawCjkStr(int x, int y, const char* s, int size);
void drawBorder();

#endif  // UI_DRAW_H
