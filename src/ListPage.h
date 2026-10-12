#ifndef LIST_PAGE_H
#define LIST_PAGE_H

#include <Arduino.h>

// 通用滚动列表页（2026-10-12，阅读菜单/书签/目录/进度跳转四种模式复用）：
//   左页 = 标题 + 可滚动选中列表（选中项实心三角 + 粗下划线，版式同主页），
//   右页 = "当前"信息区（ctx.info1/info2）+ "操作"说明区
//          （ctx.hints[count×2] 按选中项给两行提示，nullptr 给默认提示）。
//
// 版式与 HomePage/SysMenu 同一套横坐标（标题 20–66、列表 88 起每行 30、
// 页脚线 566），>16 项自动滚动（m_top 窗口随选中项移动）。
//
// 调用方（eink.ino）持有 items 文本缓冲（静态/PSRAM），按 g_subMode 拼 Ctx 后
// draw()；交互经 Result 驱动刷屏：REDRAW = 快刷、CLOSE = 调用方决定回哪层、
// PICK = 调用方按 sel() 执行动作。换行二义性窗口同 SysMenu/HomePage（400ms）。
class ListPage {
public:
  enum Result : uint8_t { NONE, REDRAW, CLOSE, PICK };

  struct Ctx {
    const char* title;            // 左页大标题（drawCjkStr 32）
    const char* const* items;     // 行文本数组（调用方保证 draw 期间有效）
    int count;
    int sel;                      // open() 时的初始选中（draw 忽略，用内部 m_sel）
    const char* const* hints;     // count×2 的两行提示，可空 = 默认提示
    const char* info1;            // 右页"当前"区第一行（可空）
    const char* info2;            // 第二行（可空）
  };

  bool isOpen() const { return m_open; }
  void open(const Ctx& ctx);      // 进列表：sel 夹取 + 滚动窗定位
  void close() { m_open = false; }
  int sel() const { return m_sel; }

  Result move(int delta);         // Prev/Next 并批后的净位移（clamp 到头不回卷）
  Result key(char c);             // 串口/Font 短按（' ' = 执行）
  void draw(uint8_t* fbL, uint8_t* fbR, const Ctx& ctx);

private:
  Result activate();

  bool m_open = false;
  int m_sel = 0;
  int m_top = 0;                  // 滚动窗口首行
  int m_count = 0;
  uint32_t m_lastKeyAt = 0;       // 换行尾字节判别窗口基准
};

#endif  // LIST_PAGE_H
