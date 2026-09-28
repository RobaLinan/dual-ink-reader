#include "SysMenu.h"

#include <ESP32epdx.h>

#include "EinkPanel.h"
#include "CjkFont.h"
#include "Storage.h"
#include "NetSvc.h"

SysMenu menu;

static const int kItems = 4;
static const uint32_t NL_WINDOW_MS = 400;   // 命令尾换行的吞掉窗口

static const char* kLabel[kItems] = {
  "WiFi 配置", "WiFi 开关", "OTA 更新", "电子书上传服务",
};

// 选中项的两行说明（屏上"执行前先看一眼这行是干嘛的"）
static const char* kHint[kItems][2] = {
  { "开配置热点，手机连上后", "网页里选 WiFi 输密码" },
  { "总开关：开=连路由器或", "开热点，关=断电省电(默认)" },
  { "开=arduino-cli 网络上传", "  + 网页 /update 传 bin" },
  { "开=网页上传书到 /books", "（需先开 WiFi）" },
};

// ---- 绘制辅助 ----

// UTF-8 解码（与 TxtBook::utf8At 同实现）：*used = 消耗字节数；非法 → '?' 吃 1 字节
static uint32_t utf8At(const char* s, int len, int* used) {
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
static void drawUtf8(int x, int y, const char* s, int maxX) {
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
static void drawCjkStr(int x, int y, const char* s, int size) {
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

static void drawBorder() {
  Paint_DrawRectangle(1, 1, EinkPanel::PAGE_W - 2, EinkPanel::PAGE_H - 2,
                      BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_2X2);
}

// 选中项右侧状态字（[开]/[关]/[热点]…）
static String rowState(int i) {
  String s = F("  ");
  switch (i) {
    case 0:
      if (net.portalUp()) s += F("[热点]");
      else if (net.staSsid()[0]) s += F("[已配]");
      else s += F("[未配]");
      break;
    case 1: s += net.wifiOn() ? F("[开]") : F("[关]"); break;
    case 2: s += net.otaOn() ? F("[开]") : F("[关]"); break;
    case 3: s += net.upOn() ? F("[开]") : F("[关]"); break;
  }
  return s;
}

// ---- 左页：菜单列表 ----

void SysMenu::drawLeft() {
  drawBorder();
  drawCjkStr(24, 20, "系统菜单", 32);
  Paint_DrawLine(24, 66, EinkPanel::PAGE_W - 24, 66,
                 BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);

  for (int i = 0; i < kItems; i++) {
    int y = 94 + i * 52;
    if (i == m_sel) {
      Paint_DrawChar(24, y, '>', &Font16, BLACK, WHITE);
      Paint_DrawLine(48, y + 19, EinkPanel::PAGE_W - 48, y + 19,
                     BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    }
    String row = kLabel[i];
    row += rowState(i);
    drawUtf8(48, y, row.c_str(), EinkPanel::PAGE_W - 32);
  }

  Paint_DrawLine(24, 316, EinkPanel::PAGE_W - 24, 316,
                 BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
  drawUtf8(24, 334, kHint[m_sel][0], EinkPanel::PAGE_W - 32);
  drawUtf8(24, 360, kHint[m_sel][1], EinkPanel::PAGE_W - 32);

  Paint_DrawLine(24, 566, EinkPanel::PAGE_W - 24, 566,
                 BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
  drawUtf8(24, 584, "j/k 选择 Enter 执行", EinkPanel::PAGE_W - 32);
  drawUtf8(24, 610, "q/m 退出菜单（方向键亦可）", EinkPanel::PAGE_W - 32);
}

// ---- 右页：状态 + 网页地址 + 提示 ----

void SysMenu::drawRight(int libCount) {
  drawBorder();
  drawCjkStr(24, 20, "状态", 32);

  int y = 68;
  auto line = [&](const String& s) {
    drawUtf8(24, y, s.c_str(), EinkPanel::PAGE_W - 32);
    y += 26;
  };

  String s = F("WiFi: ");
  s += net.stateText();
  line(s);
  NetSvc::WifiState st = net.state();
  if (st != NetSvc::OFF) {
    s = F("IP: ");
    s += net.ip();
    line(s);
    s = F("路由器: ");
    s += net.staSsid()[0] ? String(net.staSsid()) : String(F("未配置"));
    line(s);
    if (net.portalUp()) {
      s = F("热点: ");
      s += net.apSsid();
      s += F(" 192.168.4.1");
      line(s);
    }
  }
  s = F("OTA 更新: ");
  s += net.otaOn() ? String(F("开（端口 3232）")) : String(F("关"));
  line(s);
  s = F("上传服务: ");
  s += net.upOn() ? String(F("开")) : String(F("关"));
  line(s);
  s = F("书库: ");
  s += String(libCount);
  s += F(" 本");
  line(s);

  y += 14;
  drawCjkStr(24, y, "网页", 32);
  y += 46;
  if (st == NetSvc::OFF) {
    drawUtf8(24, y, "WiFi 关闭 —先开左侧 WiFi 开关", EinkPanel::PAGE_W - 32);
    y += 26;
  } else {
    s = F("配置 http://");
    s += net.ip();
    s += F("/wifi");
    line(s);
    if (net.upOn()) {
      s = F("上传 http://");
      s += net.ip();
      s += F("/upload");
      line(s);
    }
    if (net.otaOn()) {
      s = F("更新 http://");
      s += net.ip();
      s += F("/update");
      line(s);
    }
  }

  y += 14;
  drawCjkStr(24, y, "提示", 32);
  y += 46;
  drawUtf8(24, y, "改动立即生效并写 NVS", EinkPanel::PAGE_W - 32);
  y += 26;
  drawUtf8(24, y, "上传完成自动加入书库", EinkPanel::PAGE_W - 32);
  y += 26;
  drawUtf8(24, y, "合盖深睡，开盖回书页", EinkPanel::PAGE_W - 32);
}

void SysMenu::draw(uint8_t* fbL, uint8_t* fbR, int libCount) {
  // 没开过书（demo 模式）时字库可能没装载：SD 在就补一次；
  // SD 不在则跳过（CJK.begin 内部会走 Storage 退避重试，不能每次白等 3s）
  if (!CJK.ok() && Storage::mounted()) CJK.begin();

  Paint_SelectImage(fbL);
  Paint_Clear(WHITE);
  drawLeft();
  Paint_SelectImage(fbR);
  Paint_Clear(WHITE);
  drawRight(libCount);
}

// ---- 按键 ----

SysMenu::Result SysMenu::activate() {
  switch (m_sel) {
    case 0: net.startPortal(); break;                 // WiFi 配置
    case 1: net.setWifiOn(!net.wifiOn()); break;      // WiFi 开关
    case 2: net.setOtaOn(!net.otaOn()); break;        // OTA 开关
    case 3: net.setUpOn(!net.upOn()); break;          // 上传服务开关
  }
  return REDRAW;
}

SysMenu::Result SysMenu::key(char c) {
  if (!m_open) return NONE;
  if (c == '\n' || c == '\r') {
    // 换行二义性：紧跟命令字节的换行是它的尾字节（本项目习惯 "j\n" 一行发），
    // 单独敲的回车才是"执行"。首字节触发后刷新基准 → CRLF 第二字节也被吞
    uint32_t now = millis();
    if (now - m_lastKeyAt < NL_WINDOW_MS) return NONE;
    m_lastKeyAt = now;
    return activate();
  }
  m_lastKeyAt = millis();
  switch (c) {
    case 'j': case 'B':   // 下（B = 终端方向键 ↓ 序列的尾字节；ESC/[ 在 default 被吞）
      if (m_sel < kItems - 1) m_sel++;
      return REDRAW;
    case 'k': case 'A':   // 上
      if (m_sel > 0) m_sel--;
      return REDRAW;
    case ' ':             // 空格 = 执行（无尾换行歧义的回车替身）
      return activate();
    case 'q': case 'm':
      m_open = false;
      return CLOSE;
    default:
      return NONE;        // 含 ESC：吞掉方向键前缀，不当退出
  }
}
