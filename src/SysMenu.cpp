#include "SysMenu.h"

#include <ESP32epdx.h>

#include "EinkPanel.h"
#include "CjkFont.h"
#include "Storage.h"
#include "NetSvc.h"
#include "UiDraw.h"   // utf8At/drawUtf8/drawCjkStr/drawBorder（主页 HomePage 同用）

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
  { "开=网页上传/管理书库", "（/upload /library，需先开 WiFi）" },
};

// ---- 绘制辅助 ----（已抽到 UiDraw.h/.cpp：utf8At / drawUtf8 / drawCjkStr / drawBorder）

// ---- 左页：菜单列表 ----

void SysMenu::drawLeft() {
  drawBorder();

  // 标题区 + 粗分隔线
  drawCjkStr(24, 20, "系统菜单", 32);
  drawThickRule(24, 62, EinkPanel::PAGE_W - 24);

  for (int i = 0; i < kItems; i++) {
    int y = 82 + i * 52;
    bool sel = (i == m_sel);
    if (sel) {
      drawSelectArrow(24, y);          // 左侧实心三角
    }
    String row = kLabel[i];
    int textX = sel ? 42 : 36;
    drawUtf8(textX, y, row.c_str(), EinkPanel::PAGE_W - 80);
    // 状态指示点 + 状态文字
    bool on = false;
    switch (i) {
      case 0: on = net.portalUp() || net.staSsid()[0]; break;
      case 1: on = net.wifiOn(); break;
      case 2: on = net.otaOn(); break;
      case 3: on = net.upOn(); break;
    }
    int dotX = EinkPanel::PAGE_W - 70;
    drawStatusDot(dotX, y, on);
    // 状态文字
    const char* stxt = "";
    switch (i) {
      case 0: stxt = net.portalUp() ? "热点" : (net.staSsid()[0] ? "已配" : "未配"); break;
      case 1: stxt = net.wifiOn() ? "开" : "关"; break;
      case 2: stxt = net.otaOn() ? "开" : "关"; break;
      case 3: stxt = net.upOn() ? "开" : "关"; break;
    }
    drawUtf8(dotX + 12, y, stxt, EinkPanel::PAGE_W - 32);
    if (sel) {
      drawSelectBar(textX, y + 21, EinkPanel::PAGE_W - 48);
    }
  }

  // 说明区：细分隔线 + 两行说明
  drawThinRule(24, 306, EinkPanel::PAGE_W - 24);
  drawUtf8(24, 320, kHint[m_sel][0], EinkPanel::PAGE_W - 32);
  drawUtf8(24, 346, kHint[m_sel][1], EinkPanel::PAGE_W - 32);

  // 页脚：粗分隔线 + 居中提示
  drawThickRule(24, 566, EinkPanel::PAGE_W - 24);
  drawCenteredUtf8(584, "j/k 选择  Enter/空格 执行", EinkPanel::PAGE_W - 32);
  drawCenteredUtf8(610, "q/m 退出（方向键亦可）", EinkPanel::PAGE_W - 32);
}

// ---- 右页：状态 + 网页地址 + 提示 ----

void SysMenu::drawRight(int libCount) {
  drawBorder();

  // 主标题 + 粗分隔线
  drawCjkStr(24, 20, "状态", 32);
  drawThickRule(24, 62, EinkPanel::PAGE_W - 24);

  int y = 76;
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

  // 网页地址区
  y += 8;
  drawThinRule(24, y, EinkPanel::PAGE_W - 24);
  y += 12;
  drawSectionHead(24, y, "网页", 24);
  y += 38;
  if (st == NetSvc::OFF) {
    drawUtf8(24, y, "WiFi 关闭 — 先开左侧 WiFi 开关", EinkPanel::PAGE_W - 32);
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
      s = F("书库 http://");
      s += net.ip();
      s += F("/library");
      line(s);
    }
    if (net.otaOn()) {
      s = F("更新 http://");
      s += net.ip();
      s += F("/update");
      line(s);
    }
  }

  // 提示区
  y += 8;
  drawThinRule(24, y, EinkPanel::PAGE_W - 24);
  y += 12;
  drawSectionHead(24, y, "提示", 24);
  y += 38;
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
