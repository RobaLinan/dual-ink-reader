// 竖版页（EinkPanel.h: PAGE_ROTATE=90, PAGE_W=480 × PAGE_H=648）离屏回归：
// 不接硬件，直接拿库里的 GUI_Paint 验三件事 ——
//   ① Paint_NewImage(648,480,ROTATE_90) 的逻辑画布真是 480×648
//   ② 最坏情况排版（页眉截断 / 三档满页 / 悬挂顶到 HARD_PX / demo 边框）
//      一笔都不写出 38880B 显存（缓冲后面挂 4KB 哨兵带，写穿立刻可见）
//   ③ 页顶朝向 = 屏右边（ROTATE_90；270 就是反着来）
// 附带反例：逻辑 x=480 确实写穿 —— 证明 drawHeader/HARD_PX/CjkFont 三处钳位是硬需求
//（Paint_SetPixel 边界是 > 不是 ≥，旋转后 x→显存行号，库内部拦不住）。
//
// 主机端编译（不依赖 Arduino）：
//   GUI=${GUI:-$HOME/Arduino/libraries/ESP32epdx/src/GUI} \
//   g++ -std=c++17 -I"$GUI" tools/rot_layout_test.cpp "$GUI/GUI_Paint.cpp" \
//       "$GUI/font16.cpp" -o /tmp/rot_layout_test && /tmp/rot_layout_test
// 退出码 0 = 全过（输出 "all checks passed"）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "GUI_Paint.h"

static const int MEM_W = 648, MEM_H = 480;
static const uint32_t FB = (uint32_t)MEM_W * MEM_H / 8;   // 38880

// 布局常量：与 src/TxtBook.cpp 保持一致（改那边记得同步）
static const int TXT_X = 8, TXT_Y0 = 32, HARD_PX = 480 - 16, TXT_BOT = 648 - 18;
static const int PAGE_W = 480, PAGE_H = 648;

static uint8_t* fb;          // 前 FB 字节 = 显存，后面跟哨兵
static const int GUARD = 4096;
static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); \
                                            printf("   [%s:%d]\n", __FILE__, __LINE__); } } while (0)

static bool guardIntact() {
  for (int i = 0; i < GUARD; i++)
    if (fb[FB + i] != 0xFF) { printf("  guard byte %d = 0x%02x\n", i, fb[FB + i]); return false; }
  return true;
}

// 逻辑坐标 (x,y) 在显存里的字节地址（照抄 GUI_Paint.cpp case 90）
static long addrOf(int x, int y) {
  int X = MEM_W - y - 1, Y = x;
  return (long)(X / 8) + (long)Y * (MEM_W / 8);
}

int main() {
  fb = (uint8_t*)malloc(FB + GUARD);
  memset(fb, 0xFF, FB + GUARD);

  Paint_NewImage(fb, MEM_W, MEM_H, ROTATE_90, WHITE);
  printf("logical canvas: %u x %u (memory %u x %u, %u bytes/row)\n",
         Paint.Width, Paint.Height, Paint.WidthMemory, Paint.HeightMemory, Paint.WidthByte);

  CHECK(Paint.Width == PAGE_W && Paint.Height == PAGE_H,
        "logical canvas %u x %u, want %d x %d\n", Paint.Width, Paint.Height, PAGE_W, PAGE_H);
  CHECK(Paint.WidthByte == MEM_W / 8 && Paint.HeightByte == MEM_H,
        "row layout %u x %u\n", Paint.WidthByte, Paint.HeightByte);

  // 页顶朝向：逻辑左上(0,0) → 显存右上；逻辑左下(0,647) → 显存左上
  CHECK(addrOf(0, 0) == (long)(MEM_W - 1) / 8, "logical (0,0) -> %ld, want top-right\n", addrOf(0, 0));
  CHECK(addrOf(0, PAGE_H - 1) == 0, "logical (0,647) -> %ld, want top-left\n", addrOf(0, PAGE_H - 1));

  // ---- 页眉（TxtBook::drawHeader 的截断规则）+ 分隔线 ----
  int x = TXT_X;
  for (int i = 0; i < 48; i++) {              // hdr 缓冲最多 47 字符
    int adv = 11;                             // 最坏全 ASCII
    if (x + adv > PAGE_W) break;
    Paint_DrawChar(x, 8, 'W', &Font16, BLACK, WHITE);
    x += adv;
  }
  CHECK(x > TXT_X && x - 1 < PAGE_W, "header last glyph ends at x=%d (started %d)\n", x - 1, TXT_X);
  Paint_DrawLine(TXT_X, 26, PAGE_W - 1 - TXT_X, 26, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);

  // ---- 正文：三档字号满页 + 悬挂最右 token（软预算 HARD-档宽 → 悬挂顶到 HARD）----
  struct { int lineH, cjk, lines; } tiers[] = { {18, 16, 33}, {28, 24, 21}, {36, 32, 16} };
  for (auto& t : tiers) {
    int maxline = (TXT_BOT - TXT_Y0 - t.cjk) / t.lineH + 1;
    CHECK(maxline == t.lines, "tier %d maxline=%d want %d\n", t.cjk, maxline, t.lines);
    for (int l = 0; l < t.lines; l++) {
      int y = TXT_Y0 + l * t.lineH;
      int sx = TXT_X;
      while (sx + t.cjk <= TXT_X + (HARD_PX - t.cjk)) {       // 软预算内满行
        for (int d = 0; d < t.cjk; d++) Paint_SetPixel(sx + d, y + d % t.cjk, BLACK);
        sx += t.cjk;
      }
      CHECK(sx + t.cjk <= TXT_X + HARD_PX, "tier %d line %d soft end x=%d\n", t.cjk, l, sx);
      int hx = TXT_X + HARD_PX - t.cjk;       // 悬挂：token 尾巴顶到硬上限
      for (int d = 0; d < t.cjk; d++) Paint_SetPixel(hx + d, y + d, BLACK);
      Paint_DrawChar(TXT_X + HARD_PX - 11, y, 'W', &Font16, BLACK, WHITE);   // ASCII 同理
      for (int k = 0; k < t.cjk; k++) {       // 缺字方框最右/最下笔画
        Paint_SetPixel(hx + k, y, BLACK);
        Paint_SetPixel(hx, y + k, BLACK);
      }
    }
  }

  // ---- demo 页边框 + 随机实心块（DemoPage 的最右取值）----
  Paint_DrawRectangle(0, 0, PAGE_W - 1, PAGE_H - 1, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_2X2);
  uint32_t rx = 16 + (12345 * 37) % (PAGE_W - 96);
  Paint_DrawRectangle(rx, 200, rx + 60, 260, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);

  // ---- 逻辑页每个像素的显存地址都得落在 38880B 内 ----
  for (int y = 0; y <= PAGE_H - 1; y++)
    for (int x2 = 0; x2 <= PAGE_W - 1; x2 += 37)
      CHECK(addrOf(x2, y) >= 0 && addrOf(x2, y) < (long)FB, "addr(%d,%d)=%ld\n", x2, y, addrOf(x2, y));

  CHECK(guardIntact(), "guard band clobbered -> layout wrote past framebuffer\n");

  // 反例：逻辑 x=480（页眉不截断就会发生）确实写穿 —— 钳位是必需的
  memset(fb, 0xFF, FB + GUARD);
  Paint_SetPixel(PAGE_W, 100, BLACK);
  bool oob = false;
  for (int i = 0; i < GUARD; i++) if (fb[FB + i] != 0xFF) oob = true;
  printf("control: SetPixel(480,100) touches guard = %s (expected: yes)\n", oob ? "yes" : "no");
  CHECK(oob, "expected logical x=480 to be out of bounds\n");

  printf(fails ? "\n%d CHECK(s) FAILED\n" : "\nall checks passed\n", fails);
  return fails ? 1 : 0;
}
