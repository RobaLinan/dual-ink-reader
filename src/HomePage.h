#ifndef HOME_PAGE_H
#define HOME_PAGE_H

#include <Arduino.h>

// 选书主页（P7）：左页 = 书库列表（选中项 '>' + 下划线 + [在读] 标记），
// 右页 = 正在读的书 / 书库统计 / 按键说明。
//
// 定位（DESIGN §8 同一套 Result 驱动节奏，与 SysMenu 同构）：
//   Menu **短按** = 进主页（书页 → 主页，全刷）；已在主页则不动
//   Menu **长按(250ms)** = 系统菜单 SysMenu —— 菜单是"设置"，主页是"选书"，
//                          两层分开，退出菜单按来源回主页/书页
//   主页内：Prev/Next = 上下选书（连发滚动）、Font 短按 = 打开选中书
//           （选中正在读的那本 = 只回阅读页，进度原地不动）
//   串口同款：j/k（或方向键尾字节 A/B）= 上下、空格/回车 = 打开、
//             q/n = 回书页、m = 系统菜单
//
// 调用方（eink.ino）按 Result 驱动刷屏：REDRAW = 快刷、CLOSE = 回书页、
// PICK = 按 selected() 开书。draw() 每次现收 Ctx（书库可能刚被网页上传重扫）。
class HomePage {
public:
  enum Result : uint8_t { NONE, REDRAW, CLOSE, PICK };

  struct Ctx {                       // 每次画屏前由 eink.ino 现拼
    const char* const* paths;        // 书库全路径数组（/books/xxx.epub …）
    int count;                       // 书目数（0 = 空库，左页画上传提示）
    int curIdx;                      // 正在读的书（-1 = 无书/demo）
    long curPage;                    // 当前对页页号（-1 = 无书）
    const char* fontInfo;            // "classic 16px" 之类
  };

  bool isOpen() const { return m_open; }
  void open(int curIdx, int count);  // 进主页：选中项落到当前读的那本
  void close() { m_open = false; }
  int selected() const { return m_sel; }
  void select(int idx) { m_sel = idx; }   // 删书后恢复选中位置（draw 会越界夹取）

  Result move(int delta);            // Prev/Next 并批后的净位移（clamp 到库内）
  Result key(char c);                // 串口/Font 短按（' ' = 打开）
  void draw(uint8_t* fbL, uint8_t* fbR, const Ctx& ctx);

private:
  Result activate();                 // 打开选中书 → PICK
  void drawLeft(const Ctx& ctx);
  void drawRight(const Ctx& ctx);

  bool m_open = false;
  int m_sel = 0;
  int m_count = 0;                   // draw()/open() 同步；move() 据此 clamp
  uint32_t m_lastKeyAt = 0;          // 换行尾字节判别窗口基准（同 SysMenu）
};

extern HomePage home;

#endif  // HOME_PAGE_H
