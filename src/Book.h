#ifndef BOOK_H
#define BOOK_H

#include <Arduino.h>

// P4：统一的"书"接口。TxtBook（TXT）与 EpubBook（EPUB）都实现它，
// eink.ino 只面向 Book* 编程：书库扫描/切换、进度存档、字体切换全走这套。
// 深睡恢复需要章节号（仅 EPUB 有）：调用方按路径是否 .epub 决定要不要读
// s_sleepChapter 并调 EpubBook::restorePair(ch, off, page)。

class Book {
public:
  virtual ~Book() {}
  virtual bool open(const char* path) = 0;
  virtual bool isOpen() const = 0;

  // 关掉 SD 文件句柄（进度状态保留，pairFilePos 还能读）。
  // 两处必须调用：① 切书（否则旧书句柄一直占着 maxFiles 名额）；
  // ② SD 重挂载前（Storage::recover 之后旧句柄全部失效，见 EpubBook 自救路径）
  virtual void close() = 0;

  virtual void setTier(uint8_t sizePx) = 0;   // 切字号档（16/24/32）
  virtual bool setFamily(const char* name) = 0;  // 切字体家族

  // 回到最近一次 renderPair 起点：字体/字号切换后重排同一对页
  virtual bool restartPair() = 0;

  // 对页位置（进度存档）：文件位置 + 页号
  virtual long pairFilePos() const = 0;
  virtual uint32_t pairPageNo() const = 0;

  // 渲染接下来的左右两页。false = 书彻底不可用
  virtual bool renderPair(uint8_t* fbL, uint8_t* fbR) = 0;

  // —— 阅读功能（书签/目录/进度跳转，2026-10-12）——
  // 全书进度千分比 0..1000（0xFFFF = 未知）；TXT = 字节偏移/文件大小，
  // EPUB = spine 章权重（zip 条目解压尺寸）+ 章内偏移比例
  virtual uint16_t permille() = 0;
  // 跳到全书 p/10 % 处（章首对齐 EPUB 按章权重选章再章内定位；页号为估计值）
  virtual bool jumpPermille(uint16_t p) = 0;
  // 目录条目数（0 = 无目录）；EPUB 解析 NCX/nav 失败时退化"第N章"合成目录
  virtual int tocCount() = 0;
  virtual bool tocTitle(int idx, char* out, int n) = 0;
  virtual bool jumpToToc(int idx) = 0;   // 跳到目录第 idx 条（章首）
};

#endif  // BOOK_H
