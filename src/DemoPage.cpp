#include "DemoPage.h"
#include "EinkPanel.h"
#include <ESP32epdx.h>

void drawDemoPage(uint8_t* buf, uint32_t page, const char* side, bool fullMode) {
  Paint_SelectImage(buf);
  Paint_Clear(WHITE);
  Paint_DrawRectangle(0, 0, EinkPanel::WIDTH - 1, EinkPanel::HEIGHT - 1,
                      BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_2X2);
  Paint_DrawString_EN(16, 20, side, &Font16, BLACK, WHITE);
  Paint_DrawString_EN(16, 60, fullMode ? "mode: FULL" : "mode: FAST",
                      &Font16, BLACK, WHITE);
  Paint_DrawNum(16, 100, page, &Font16, BLACK, WHITE);
  uint32_t x = 16 + (page * 37) % (EinkPanel::WIDTH - 96);
  Paint_DrawRectangle(x, 200, x + 60, 260, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
}
