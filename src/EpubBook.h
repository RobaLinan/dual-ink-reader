#ifndef EPUB_BOOK_H
#define EPUB_BOOK_H

#include <Arduino.h>
#include "Book.h"
#include "TxtBook.h"
#include "EpubZip.h"

// P4：EPUB 流式阅读（DESIGN §4 P4 行：miniz + tinyxml2 锁版本 + 书库 + 进度）。
//
// 架构 = "一章一转换"，复用 TxtBook 排版引擎，绝不整本常驻：
//   open()        = 打开 zip → container.xml → OPF → spine（章列表）
//   loadChapter() = miniz 流式解压该章 XHTML 到片内 LittleFS 临时文件（坑 6；
//                   坑 18：EPUB 不写 SD，写 SD 会物理锁卡）→ 整章读进 PSRAM →
//                   tinyxml2 解析 → 抽正文写 LittleFS 临时文本
//                   → TxtBook 打开这个临时文本（翻页/中英混排/字体全家免费）
//   renderPair()  = 章末（hitEof）下一对页自动进下一章；右页中道结束画空白页
// 峰值 RAM = 章节缓冲 + DOM（都 PSRAM，DOM 池块打了 PSRAM 补丁见 VENDOR.md），
// 解析完即释放；验收线：>280KB 章节读完一章不崩不 OOM。

class EpubBook : public Book {
public:
  static const uint16_t MAXCH = 256;   // spine 章数上限（防御）

  bool open(const char* path) override;
  bool isOpen() const override { return m_ok; }
  void close() override;                // 关 m_txt + m_zip 句柄（切书/SD 重挂前）

  void setTier(uint8_t sizePx) override;
  bool setFamily(const char* name) override;

  bool restartPair() override;         // 字体切换回跳当前对页（章内位置）

  long pairFilePos() const override { return m_pairStart; }
  uint32_t pairPageNo() const override { return m_pairStartPage; }
  uint16_t pairChapter() const { return m_pairStartCh; }   // 进度存档扩展项

  bool renderPair(uint8_t* fbL, uint8_t* fbR) override;

  // 深睡恢复：切到第 ch 章 + 章内偏移 off + 页号（TxtBook 语义同款）
  bool restorePair(uint16_t ch, long off, uint32_t page);

  const char* title() const { return m_title; }
  uint16_t chapterCount() const { return m_nChapters; }
  uint16_t currentChapter() const { return m_chapter; }

private:
  bool loadChapter(uint16_t idx);      // 解压+解析+抽文本+交给 TxtBook
  bool nextChapter();                  // 推进一章（读完回卷第一章）
  bool parseOpf(const char* opfPath);  // 解析 OPF：标题/manifest/spine
  bool convertChapterToText();         // XHTML 临时文件 → 纯文本临时文件
  bool extractWithRecovery(const char* zipPath, const char* outPath);  // 解压 + SD 读自救

  EpubZip m_zip;
  TxtBook m_txt;                       // 排版引擎（内部组合，接口对齐 Book）

  bool m_ok = false;
  bool m_chapterDone = false;          // 当前章翻完，下一对页要进下一章
  uint16_t m_chapter = 0;
  uint16_t m_nChapters = 0;
  uint32_t m_page = 0;                 // 全书连续页号（跨章不重置）

  // 最近一次 renderPair 起点（restartPair / 进度存档）
  long m_pairStart = -1;
  uint32_t m_pairStartPage = 0;
  uint16_t m_pairStartCh = 0;

  // 字体/字号随章保存（loadChapter 里对 m_txt 重放）
  uint8_t m_tier = 16;
  char m_family[16] = "classic";

  char m_title[32] = "";
  char m_opfDir[48] = "";              // OPF 所在 zip 内目录（href 拼接用）
  char m_chapterPath[MAXCH][64];       // 每章 zip 内路径（256×64B = 16KB RAM）
  char m_path[64] = "";                // 当前书路径（SD 重挂后重开 zip 用）
  bool m_sdDead = false;               // 读探针判死：本会话不再探 SD（防连环超时）
};

#endif  // EPUB_BOOK_H
