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
};

#endif  // BOOK_H
