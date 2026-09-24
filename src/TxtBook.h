#ifndef TXT_BOOK_H
#define TXT_BOOK_H

#include <Arduino.h>
#include <FS.h>

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
class TxtBook {
public:
  // 挂载 SD 并打开 path。false = 卡/文件问题（不 fatal，调用方退 demo 页）
  bool open(const char* path);
  bool isOpen() const { return m_ok; }

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
  long pairFilePos() const { return m_pairStart; }
  uint32_t pairPageNo() const { return m_pairStartPage; }
  bool restorePair(long off, uint32_t page);  // seek 回对页起点，调用方负责重画

  // 渲染接下来的左右两页到 fbL/fbR。
  // false = 渲染不出内容（此后 isOpen() 变 false）
  bool renderPair(uint8_t* fbL, uint8_t* fbR);

private:
  bool renderPage(uint8_t* fb, uint32_t pageNo);

  File m_file;
  bool m_ok = false;
  uint32_t m_page = 0;   // 下一个要渲染的页号
  long m_pairStart = -1;      // 最近一次 renderPair 的文件起点（restartPair 用）
  uint32_t m_pairStartPage = 0;
  char m_title[32];      // 页眉用（取自文件名）
};

#endif  // TXT_BOOK_H
