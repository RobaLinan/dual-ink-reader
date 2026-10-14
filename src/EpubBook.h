#ifndef EPUB_BOOK_H
#define EPUB_BOOK_H

#include <Arduino.h>
#include "Book.h"
#include "TxtBook.h"
#include "EpubZip.h"

// 前向声明：目录解析遍历 tinyxml2 DOM（实现里才包含 tinyxml2 头）
namespace tinyxml2 { class XMLElement; }

// P4：EPUB 流式阅读（DESIGN §4 P4 行：miniz + tinyxml2 锁版本 + 书库 + 进度）。
//
// 架构 = "一章一转换"，复用 TxtBook 排版引擎，绝不整本常驻：
//   open()        = 打开 zip → container.xml → OPF → spine（章列表）
//   loadChapter() = miniz 流式解压该章 XHTML 到片内 LittleFS 临时文件（坑 6；
//                   坑 18：EPUB 不写 SD，写 SD 会物理锁卡）→ 整章读进 PSRAM →
//                   tinyxml2 解析 → 抽正文写 LittleFS 临时文本
//                   → TxtBook 打开这个临时文本（翻页/中英混排/字体全家免费）
//   renderPair()  = 章末（hitEof）下一对页自动进下一章；右页中道结束画空白页
// 插图：<img>/<image>(svg) 在转换期写成 [[img:zip内路径]] 标记行，TxtBook 排版
// 到标记时回调本类：解压图片条目到 LittleFS 临时文件 → EpubImg 解码（JPEG 走
// S3 ROM 硬件解码器 / PNG 走 PNGdec）→ 等比缩放 + 4×4 Bayer 抖动落帧缓冲；
// 量高度占位，页中放不下整图推下页，页首硬压进一页。GIF/WebP/渐进 JPEG 跳过。
// 峰值 RAM = 章节缓冲 + DOM（都 PSRAM，DOM 池块打了 PSRAM 补丁见 VENDOR.md），
// 解析完即释放；验收线：>280KB 章节读完一章不崩不 OOM。
//
// 双核章节缓存（DESIGN §13，2026-10-13）：loadChapter 同步路径（解压+解析+落盘）
// 大章节几秒到十几秒，core0 全程闲着 → 后台预取任务把前/后章提前转换进 3 个
// LittleFS 槽位轮换，主核命中 = 打开文件（毫秒级）。槽位状态机与并发约束见
// cpp 文件"双核章节缓存"一节。

class EpubBook : public Book {
public:
  static const uint16_t MAXCH = 256;   // spine 章数上限（防御）
  static const int NSLOT = 3;          // 章节缓存槽位数（文本，见 DESIGN §13）

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

  // —— 阅读功能（2026-10-12，见 Book.h 注释）——
  uint16_t permille() override;
  bool jumpPermille(uint16_t p) override;
  int tocCount() override { return m_nToc ? m_nToc : m_nChapters; }
  bool tocTitle(int idx, char* out, int n) override;
  bool jumpToToc(int idx) override;

  // zip 条目 → spine 章号（-1 = 不是章节；forEach 回调/目录匹配共用）
  int chapterIndexOf(const char* zipPath) const;

  // setup() 早期（net.begin 启动 WiFi 之前）调用一次：建同步原语（mutex/queue）。
  // core0 任务延迟到 pfEnsure()（由 kickAdjacent 调用、WiFi 关闭时）创建 ——
  // WiFi 连接期间 xTaskCreatePinnedToCore 向 core0 挂任务会撞跨核 scheduler
  // 自旋锁，导致开机卡死（坑 25）
  static void prefetchInit();

private:
  bool loadChapter(uint16_t idx);      // 命中槽位直接开文件；未命中走同步转换
  bool nextChapter();                  // 推进一章（读完回卷第一章）
  bool parseOpf(const char* opfPath);  // 解析 OPF：标题/manifest/spine + 目录(NCX/nav)
  bool convertChapterToText(const char* docDir, const char* xhtmlPath,
                            const char* txtPath);   // XHTML 临时文件 → 纯文本临时文件
  bool extractWithRecovery(const char* zipPath, const char* outPath);  // 解压 + SD 读自救

  // —— 双核章节缓存（DESIGN §13）：core0 预取任务预转换前后章，翻章 = 开文件 ——
  struct SlotState { int8_t state; int16_t ch; };  // 0=IDLE 1=READY 2=WRITING；ch=-1 无
  struct PfReq { uint32_t gen; uint16_t ch; int8_t slot; };
  struct SdMuGuard;                     // SD 访问锁 RAII（递归互斥，定义在 cpp）

  int acquireSlot(uint16_t idx);        // 主核：READY 命中 / 等预取落定 / 驱逐 + 同步转换
  bool syncConvert(uint16_t idx, int slot);        // 主核慢路径（extractWithRecovery）
  void evictSlot(int slot);             // READY → IDLE（文件不删，复用时 FILE_WRITE 截断）
  void clearSlots();                    // 全清 + 删槽位文件（open/close 换书时）
  void invalidateOtherSlots();          // 跳转后清非当前章槽位（WRITING 不动）
  void kickAdjacent();                  // 主核：前后章预取入队（WiFi/流式字库门控）
  void pfEnsure();                      // 惰性建 s_mu/s_sdMu/队列/core0 任务
  bool pfMine(int slot, uint32_t gen, uint16_t ch) const;  // 任务：槽位仍归我这单？
  bool pfConvert(uint16_t idx, int slot, uint32_t gen);    // 预取任务体（core0）
  void pfFinish(int slot, uint32_t gen, uint16_t ch, bool ok);
  static void pfTask(void* arg);

  static SemaphoreHandle_t s_mu;        // 槽位状态互斥
  static SemaphoreHandle_t s_sdMu;      // SD 访问串行（递归）
  static TaskHandle_t s_pfTask;         // core0 预取任务（单例）
  static QueueHandle_t s_pfQueue;       // 预取请求队列（深度 4）
  static SlotState s_slot[NSLOT];       // 槽位状态表
  static EpubBook* s_pfBook;            // 任务上下文（固件单书假设）

  // 目录（EPUB2 NCX / EPUB3 nav.xhtml；都没有 → tocCount 退化为"第N章"合成目录）
  bool parseNcxToc(const char* zipPath, const char* baseDir);
  bool parseNavToc(const char* zipPath, const char* baseDir);
  void walkNcx(const tinyxml2::XMLElement* navPoint, const char* baseDir);
  void walkNav(const tinyxml2::XMLElement* ol, const char* baseDir);
  void addToc(uint16_t ch, const char* title);

  // 进度%：spine 章权重（zip 条目解压尺寸）一次 O(N) 扫描惰性建表
  bool ensureChSizes();
  static void chSizeCb(const char* path, uint32_t size, void* ctx);
  uint32_t estTotalPages();            // 按已渲染文本平均页字节估全书页数（页号用）

  // —— 插图（TxtBook 排版到图标记时回调；static trampoline 挂到 this）——
  bool measureImage(const char* zipPath, int maxW, int* outH);  // 探尺寸算占位行数
  bool drawImage(const char* zipPath, int x, int y, int maxW, int maxH);  // 解码绘制
  bool ensureImageTmp(const char* zipPath);  // 解压到 LittleFS 临时文件（同路径复用）
  static bool measTramp(void* ctx, const char* p, int w, int* h) {
    return ((EpubBook*)ctx)->measureImage(p, w, h);
  }
  static bool drawTramp(void* ctx, const char* p, int x, int y, int w, int h) {
    return ((EpubBook*)ctx)->drawImage(p, x, y, w, h);
  }

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
  int8_t m_curSlot = -1;               // m_txt 正打开的文本槽（-1 = 无）
  uint32_t m_gen = 0;                  // 书会话代际：open/close 自增，预取单陈旧判据
  char m_imgPath[160] = "";            // 已解压进 TEMP_IMG 的 zip 路径（缓存键）
  bool m_imgCached = false;
  bool m_imgOk = false;

  // —— 目录（parseOpf 末段填充；m_nToc=0 时 tocCount/tocTitle 退化"第N章"合成）——
  uint16_t m_nToc = 0;                 // NCX/nav 解析出的目录条目数（≤ MAXCH）
  uint16_t m_tocCh[MAXCH];             // 每条目录 → spine 章号（512B）
  char* m_tocBuf = nullptr;            // PSRAM 标题行（MAXCH×48B），惰性分配

  // —— 进度%权重（ensureChSizes 惰性一次建表）——
  uint32_t m_chSize[MAXCH];            // 每章 zip 条目解压尺寸（权重，1KB）
  uint32_t m_totalSize = 0;            // 权重总和
  bool m_sizesOk = false;
};

#endif  // EPUB_BOOK_H
