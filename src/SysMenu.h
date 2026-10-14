#ifndef SYS_MENU_H
#define SYS_MENU_H

#include <Arduino.h>

// 系统菜单（DESIGN §8）：墨水屏呈现 + 串口导航，四项：
//   WiFi 配置 / WiFi 开关 / OTA 更新开关 / 电子书上传服务开关
//
// 交互（P6 4 键落地后 BTN_PREV/NEXT/MENU 绑同一 key() 接口）：
//   菜单外：m 开菜单；菜单内 j/k（或方向键尾字节 A/B）上下、Enter/空格执行、
//   q/m 退出。字符立即执行；紧跟命令的换行视为尾字节吞掉（400ms 窗），
//   单独敲的回车才是"执行" —— 兼容本项目 "b\n" 一行发的串口习惯。
//
// 开着菜单时 eink.ino 暂停自动翻页，每次导航/动作用快刷、开菜单用全刷；
// 执行动作全部委托 NetSvc（立即生效并写 NVS）。
class SysMenu {
public:
  enum Result : uint8_t { NONE, REDRAW, CLOSE };   // 调用方按此驱动刷屏节奏

  bool isOpen() const { return m_open; }
  void open() { m_open = true; m_sel = 0; m_lastKeyAt = millis(); }
  void close() { m_open = false; }

  Result key(char c);                              // 菜单内按键分发
  void draw(uint8_t* fbL, uint8_t* fbR, int libCount);  // 画进左右 draw 缓冲

  void (*onFullEvery)() = nullptr;   // "全刷间隔"项执行回调（eink.ino 设置）
  int fullEvery = 5;                 // 当前全刷间隔值（eink.ino 同步，draw 显示用）

private:
  Result activate();                               // 执行当前选中项
  void drawLeft();
  void drawRight(int libCount);

  bool m_open = false;
  int m_sel = 0;
  uint32_t m_lastKeyAt = 0;    // 换行尾字节判别窗口的基准
};

extern SysMenu menu;

#endif  // SYS_MENU_H
