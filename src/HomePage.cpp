#include "HomePage.h"

#include <ESP32epdx.h>

#include "EinkPanel.h"
#include "CjkFont.h"
#include "Storage.h"
#include "UiDraw.h"

HomePage home;

// === 版式（竖版页 480×648，见 EinkPanel.h；与 SysMenu 共用同一套横坐标）===
//   标题 20–66 → 列表 88 起、每行 30（16 本（书库容量上限）→ 末行 538）
//   → 页脚线 566 + 两行按键提示 584/610（页底 648 留 22px 余量）
static const int ROW_TOP = 88;
static const int ROW_STEP = 30;
static const int FOOT_RULE_Y = 566;
static const int MAX_ROWS = 16;        // 与 eink.ino s_lib[16] 容量一致
static const uint32_t NL_WINDOW_MS = 400;   // 命令尾换行吞掉窗口（同 SysMenu）

// "/books/alice.epub" → "alice"：去掉目录前缀与扩展名。
// 静态缓冲：一次只在一处用（画多行要逐行画，不能同表达式里用两次）
static const char* baseName(const char* p) {
  static char buf[64];
  const char* s = strrchr(p, '/');
  s = s ? s + 1 : p;
  snprintf(buf, sizeof(buf), "%s", s);
  char* dot = strrchr(buf, '.');
  if (dot && dot != buf) *dot = '\0';
  return buf;
}

// 后缀比较（大小写不敏感，与 eink.ino hasExt 同义）
static bool hasExt(const char* name, const char* ext) {
  size_t nl = strlen(name), el = strlen(ext);
  if (nl <= el) return false;
  const char* s = name + nl - el;
  for (size_t i = 0; i < el; i++)
    if (tolower((unsigned char)s[i]) != tolower((unsigned char)ext[i])) return false;
  return true;
}

// ---- 左页：书库列表 ----

void HomePage::drawLeft(const Ctx& ctx) {
  drawBorder();

  // 标题区："书库" + 计数 + 粗分隔线
  drawCjkStr(24, 20, "书库", 32);
  char buf[48];
  snprintf(buf, sizeof(buf), "共 %d 本", ctx.count);
  drawUtf8(112, 34, buf, EinkPanel::PAGE_W - 32);
  drawThickRule(24, 62, EinkPanel::PAGE_W - 24);

  if (ctx.count <= 0) {
    drawUtf8(24, ROW_TOP, "书库是空的 —— 用系统菜单里的", EinkPanel::PAGE_W - 32);
    drawUtf8(24, ROW_TOP + 26, "电子书上传服务把书放进来", EinkPanel::PAGE_W - 32);
  }

  int n = ctx.count < MAX_ROWS ? ctx.count : MAX_ROWS;
  for (int i = 0; i < n; i++) {
    int y = ROW_TOP + i * ROW_STEP;
    bool sel = (i == m_sel);
    if (sel) {
      drawSelectArrow(24, y);          // 左侧实心三角
    }
    String row = baseName(ctx.paths[i]);
    row += hasExt(ctx.paths[i], ".epub") ? F("  [epub]") : F("  [txt]");
    if (i == ctx.curIdx) row += F("  [在读]");
    int textX = sel ? 42 : 36;         // 选中项让位给三角
    drawUtf8(textX, y, row.c_str(), EinkPanel::PAGE_W - 32);
    if (sel) {
      drawSelectBar(textX, y + 21, EinkPanel::PAGE_W - 48);  // 粗下划线
    }
  }

  // 页脚：粗分隔线 + 居中提示
  drawThickRule(24, FOOT_RULE_Y, EinkPanel::PAGE_W - 24);
  drawCenteredUtf8(584, "Prev/Next 选择  Font 短按打开", EinkPanel::PAGE_W - 32);
  drawCenteredUtf8(610, "Font 长按删除  Menu 长按系统菜单", EinkPanel::PAGE_W - 32);
}

// ---- 右页：正在读 + 书库统计 + 按键说明 ----

void HomePage::drawRight(const Ctx& ctx) {
  drawBorder();

  // 主标题 + 粗分隔线
  drawCjkStr(24, 20, "主页", 32);
  drawThickRule(24, 62, EinkPanel::PAGE_W - 24);

  char buf[64];

  // 正在读
  drawSectionHead(24, 76, "正在读", 24);
  int y = 110;
  if (ctx.curIdx >= 0 && ctx.curIdx < ctx.count) {
    drawUtf8(24, y, baseName(ctx.paths[ctx.curIdx]), EinkPanel::PAGE_W - 32);
    y += 26;
    if (ctx.curPage >= 0) {
      snprintf(buf, sizeof(buf), "第 %ld 对页", ctx.curPage);
      drawUtf8(24, y, buf, EinkPanel::PAGE_W - 32);
      y += 26;
    }
  } else {
    drawUtf8(24, y, "（还没有打开过书）", EinkPanel::PAGE_W - 32);
    y += 26;
  }
  drawUtf8(24, y, ctx.fontInfo, EinkPanel::PAGE_W - 32);

  // 书库统计
  drawThinRule(24, 200, EinkPanel::PAGE_W - 24);
  drawSectionHead(24, 212, "书库", 24);
  int nt = 0, ne = 0;
  for (int i = 0; i < ctx.count; i++) { if (hasExt(ctx.paths[i], ".epub")) ne++; else nt++; }
  snprintf(buf, sizeof(buf), "TXT %d 本   EPUB %d 本", nt, ne);
  drawUtf8(24, 246, buf, EinkPanel::PAGE_W - 32);
  drawUtf8(24, 272, "新书：Menu 长按 → 电子书上传服务", EinkPanel::PAGE_W - 32);

  // 按键说明
  drawThinRule(24, 310, EinkPanel::PAGE_W - 24);
  drawSectionHead(24, 322, "按键", 24);
  static const char* kHint[7] = {
    "Prev/Next  上下选择",
    "Font 短按  打开选中书",
    "Font 长按  删除选中书",
    "选中当前书 = 直接回阅读页",
    "Menu 短按  回到本页（书页时）",
    "Menu 长按  系统菜单",
    "合盖深睡，开盖回上次状态",
  };
  for (int i = 0; i < 7; i++)
    drawUtf8(24, 358 + i * 26, kHint[i], EinkPanel::PAGE_W - 32);
}

void HomePage::draw(uint8_t* fbL, uint8_t* fbR, const Ctx& ctx) {
  // 没开过书（demo 模式）时字库可能没装载：SD 在就补一次；
  // SD 不在则跳过（CJK.begin 内部走 Storage 退避重试，别每次白等 3s）
  if (!CJK.ok() && Storage::mounted()) CJK.begin();

  // 书库可能刚被网页上传重扫 → 选中项越界先夹回来
  m_count = ctx.count;
  if (m_sel >= m_count) m_sel = m_count > 0 ? m_count - 1 : 0;
  if (m_sel < 0) m_sel = 0;

  Paint_SelectImage(fbL);
  Paint_Clear(WHITE);
  drawLeft(ctx);
  Paint_SelectImage(fbR);
  Paint_Clear(WHITE);
  drawRight(ctx);
}

// ---- 状态 ----

void HomePage::open(int curIdx, int count) {
  m_count = count;
  m_sel = (curIdx >= 0 && curIdx < count) ? curIdx : 0;
  m_open = true;
  // 同 SysMenu::open：进主页时立换行窗基准 —— 否则"n\n"的尾字节 \n 会落在
  // 400ms 窗外被当成"单独回车 = 打开选中书"，刚进主页就误开当前书
  m_lastKeyAt = millis();
}

HomePage::Result HomePage::move(int delta) {
  if (m_count <= 0) return NONE;
  long n = (long)m_sel + delta;
  if (n < 0) n = 0;
  if (n >= m_count) n = m_count - 1;        // clamp（同 SysMenu：到头不回卷）
  if (n == m_sel) return NONE;
  m_sel = (int)n;
  return REDRAW;
}

HomePage::Result HomePage::activate() {
  return m_count > 0 ? PICK : NONE;         // 空库无从打开
}

HomePage::Result HomePage::key(char c) {
  if (!m_open) return NONE;
  if (c == '\n' || c == '\r') {
    // 换行二义性（同 SysMenu）：紧跟命令字节的换行是它的尾字节（本项目习惯
    // "j\n" 一行发），单独敲的回车才是"打开" —— 否则每次 j\n 都会误开书
    uint32_t now = millis();
    if (now - m_lastKeyAt < NL_WINDOW_MS) return NONE;
    m_lastKeyAt = now;
    return activate();
  }
  m_lastKeyAt = millis();
  switch (c) {
    case 'j': case 'B':   // 下（B = 终端方向键 ↓ 尾字节；ESC/[ 在 default 被吞）
      return move(1);
    case 'k': case 'A':   // 上
      return move(-1);
    case ' ':             // 空格 = 打开（Font 短按也走这里）
      return activate();
    case 'q': case 'n':   // 回阅读页（n = 进/出主页同一键）
      m_open = false;
      return CLOSE;
    default:
      return NONE;        // 含 ESC、f/s/b/h/a：主页期间不响应
  }
}
