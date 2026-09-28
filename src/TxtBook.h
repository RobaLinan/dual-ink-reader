#ifndef TXT_BOOK_H
#define TXT_BOOK_H

#include <Arduino.h>
#include <FS.h>
#include "Book.h"

// P3：SD 卡纯文本阅读（中英混排，DESIGN §5.5）
//   open()       = Storage 挂载 + 打开文件 + 字库加载（CJK.begin）+ 串口 dump 头部
//   renderPair() = 连续渲染左右两页（N / N+1），文件位置持续推进
//
// 排版按像素宽，三档字号（TxtBook::setTier，改默认见 TXT_SIZE_PX）：
//   S(16) Font16 11px + Unifont 16px，行高 18，24 行/页
//   M(24) Font24 17px + Unifont 1.5×，行高 28，15 行/页
//   L(32) Font16 2× 放大 22px + Unifont 2×，行高 36，12 行/页
//   拉丁按词断（词内不断），CJK 每字独立 token 两端可断 + 避头尾
//   （行首禁则悬挂到行尾；放不下时把上一 token 推挤到下一行同行）；
//   页尾半行回退文件位置（不丢不重），EOF 回卷从头再翻。
// UTF-8：分词处解码成码点（绝不按字节切），码点级字宽与绘制路径一致。
#ifndef TXT_SIZE_PX
#define TXT_SIZE_PX 16     // 默认字号档（16/24/32）：改这里再编译即可换档
#endif
class TxtBook : public Book {
public:
  // 挂载 SD 并打开 path。false = 卡/文件问题（不 fatal，调用方退 demo 页）
  bool open(const char* path);
  bool isOpen() const { return m_ok; }
  void close() override;               // 关文件句柄（切书/SD 重挂载前）；进度状态保留

  // 切字号档（16/24/32），只影响之后渲染的页；非法值回退 16。
  // 家族包模式下会按新档位重载包（该档缺文件 → 回 classic）
  void setTier(uint8_t sizePx);

  // 切字体家族（名字来自 CJK.familyName()；"classic" = Font16/24+Unifont）。
  // SD 里没有对应包 → 回 classic 并返回 false。几何随包度量联动
  bool setFamily(const char* name);

  // 回到最近一次 renderPair 的起点：字体/字号切换后调用，随后 renderPair
  // 即用新字体重排同一对页（Kindle 式切换即预览）
  bool restartPair();

  // 深睡存档/恢复（RTC 内存跨睡保留）：屏上对页的文件位置与页号
  long pairFilePos() const override { return m_pairStart; }
  uint32_t pairPageNo() const override { return m_pairStartPage; }
  // 当前章内文件实时位置（EpubBook 直接调 renderPage 步进，不走 renderPair，
  // 需要用这个取"下一对页起点"）
  long curFilePos() const { return m_file ? (long)m_file.position() : -1; }
  bool restorePair(long off, uint32_t page);  // seek 回对页起点，调用方负责重画

  // 渲染接下来的左右两页到 fbL/fbR。
  // false = 渲染不出内容（此后 isOpen() 变 false）
  bool renderPair(uint8_t* fbL, uint8_t* fbR);

  // —— P4 EPUB 复用接口（EpubBook 组合 TxtBook 排版引擎）——
  // 渲染单页（renderPair 的内部步进，EPUB 章末要在左右页之间插空白页）
  bool renderPage(uint8_t* fb, uint32_t pageNo);

  // 空白页（只有页眉+分隔线）：EPUB 章节在右页中间结束时用
  void renderBlank(uint8_t* fb, uint32_t pageNo);

  // 循环开关：true（默认）= TXT 翻完回卷从头；false = EPUB 章末停在 EOF，
  // hitEof() 置位，不自动回卷（由 EpubBook 切下一章）
  void setLoop(bool loop) { m_loop = loop; }
  bool hitEof() const { return m_hitEof; }

  // 覆盖页眉标题（EPUB 用书标题替代临时文件名）；nullptr 恢复用文件名
  void setTitle(const char* t);

  // 关掉串口头部 dump（EPUB 章节切换太频繁，日志会刷屏）
  void setQuiet(bool q) { m_quiet = q; }

private:

  File m_file;
  bool m_ok = false;
  bool m_loop = true;    // false = EPUB 模式：EOF 停住不回头（setLoop 切换）
  bool m_hitEof = false; // 上一次渲染触碰过文件尾（EPUB 章末推进用）
  bool m_quiet = false;  // true = open() 不 dump 头部 400B
  uint32_t m_page = 0;   // 下一个要渲染的页号
  long m_pairStart = -1;      // 最近一次 renderPair 的文件起点（restartPair 用）
  uint32_t m_pairStartPage = 0;
  bool m_titleOverridden = false;
  char m_title[32];      // 页眉用（取自文件名，可被 setTitle 覆盖）
};

#endif  // TXT_BOOK_H
