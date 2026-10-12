#include "ListPage.h"

#include <ESP32epdx.h>

#include "EinkPanel.h"
#include "CjkFont.h"
#include "Storage.h"
#include "UiDraw.h"

// === 版式（竖版页 480×648，与 HomePage/SysMenu 共用同一套横坐标）===
static const int ROW_TOP = 88;
static const int ROW_STEP = 30;
static const int FOOT_RULE_Y = 566;
static const int MAX_ROWS = 16;
static const uint32_t NL_WINDOW_MS = 400;

void ListPage::draw(uint8_t* fbL, uint8_t* fbR, const Ctx& ctx) {
  // 没开过书（demo 模式）时字库可能没装载：SD 在就补一次（同 SysMenu/HomePage）
  if (!CJK.ok() && Storage::mounted()) CJK.begin();

  // 书库/书签数可能刚变 → 选中项越界先夹回来；滚动窗跟随选中项
  m_count = ctx.count;
  if (m_sel >= m_count) m_sel = m_count > 0 ? m_count - 1 : 0;
  if (m_sel < 0) m_sel = 0;
  if (m_sel < m_top) m_top = m_sel;
  if (m_sel >= m_top + MAX_ROWS) m_top = m_sel - MAX_ROWS + 1;

  Paint_SelectImage(fbL);
  Paint_Clear(WHITE);
  drawBorder();
  drawCjkStr(24, 20, ctx.title, 32);
  drawThickRule(24, 62, EinkPanel::PAGE_W - 24);

  if (ctx.count <= 0) {
    drawUtf8(24, ROW_TOP, "（无条目）", EinkPanel::PAGE_W - 32);
  }
  int vis = ctx.count - m_top;
  if (vis > MAX_ROWS) vis = MAX_ROWS;
  for (int i = 0; i < vis; i++) {
    int idx = m_top + i;
    int y = ROW_TOP + i * ROW_STEP;
    bool sel = (idx == m_sel);
    if (sel) drawSelectArrow(24, y);
    int textX = sel ? 42 : 36;
    drawUtf8(textX, y, ctx.items[idx], EinkPanel::PAGE_W - 32);
    if (sel) drawSelectBar(textX, y + 21, EinkPanel::PAGE_W - 48);
  }

  drawThickRule(24, FOOT_RULE_Y, EinkPanel::PAGE_W - 24);
  drawCenteredUtf8(584, "Prev/Next 选择  Font 短按执行", EinkPanel::PAGE_W - 32);
  drawCenteredUtf8(610, "Menu 返回", EinkPanel::PAGE_W - 32);

  Paint_SelectImage(fbR);
  Paint_Clear(WHITE);
  drawBorder();
  drawCjkStr(24, 20, "说明", 32);
  drawThickRule(24, 62, EinkPanel::PAGE_W - 24);

  int y = 76;
  if (ctx.info1 && ctx.info1[0]) {
    drawSectionHead(24, y, "当前", 24);
    y += 34;
    drawUtf8(24, y, ctx.info1, EinkPanel::PAGE_W - 32);
    y += 26;
    if (ctx.info2 && ctx.info2[0]) {
      drawUtf8(24, y, ctx.info2, EinkPanel::PAGE_W - 32);
      y += 26;
    }
  }
  y += 8;
  drawThinRule(24, y, EinkPanel::PAGE_W - 24);
  y += 12;
  drawSectionHead(24, y, "操作", 24);
  y += 34;
  const char* h1 = "Font 短按  打开 / 跳转";
  const char* h2 = "Menu  返回上级";
  if (ctx.hints && m_sel < ctx.count) {
    h1 = ctx.hints[m_sel * 2];
    h2 = ctx.hints[m_sel * 2 + 1];
  }
  drawUtf8(24, y, h1, EinkPanel::PAGE_W - 32);
  y += 26;
  if (h2 && h2[0]) drawUtf8(24, y, h2, EinkPanel::PAGE_W - 32);
}

void ListPage::open(const Ctx& ctx) {
  m_count = ctx.count;
  m_sel = (ctx.sel >= 0 && ctx.sel < ctx.count) ? ctx.sel : 0;
  m_top = m_sel >= MAX_ROWS ? m_sel - MAX_ROWS + 1 : 0;
  m_open = true;
  // 同 SysMenu::open：进列表时立换行窗基准，防"命令+\n"的尾字节被当执行
  m_lastKeyAt = millis();
}

ListPage::Result ListPage::move(int delta) {
  if (m_count <= 0) return NONE;
  long n = (long)m_sel + delta;
  if (n < 0) n = 0;
  if (n >= m_count) n = m_count - 1;
  if (n == m_sel) return NONE;
  m_sel = (int)n;
  return REDRAW;
}

ListPage::Result ListPage::activate() {
  return m_count > 0 ? PICK : NONE;
}

ListPage::Result ListPage::key(char c) {
  if (!m_open) return NONE;
  if (c == '\n' || c == '\r') {
    uint32_t now = millis();
    if (now - m_lastKeyAt < NL_WINDOW_MS) return NONE;
    m_lastKeyAt = now;
    return activate();
  }
  m_lastKeyAt = millis();
  switch (c) {
    case 'j': case 'B':   // 下（B = 终端方向键 ↓ 尾字节）
      return move(1);
    case 'k': case 'A':   // 上
      return move(-1);
    case ' ':             // 空格 = 执行
      return activate();
    case 'q': case 'm':   // m 与 Menu 键同义（阅读菜单模式下 q/m 退回书页）
      m_open = false;
      return CLOSE;
    default:
      return NONE;
  }
}
