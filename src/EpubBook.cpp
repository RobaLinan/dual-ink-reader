#include "EpubBook.h"
#include "Storage.h"
#include "SD_MMC.h"
#include "EinkPanel.h"
#include "CjkFont.h"
#include "EpubImg.h"
#include "NetSvc.h"   // net.wifiOn()：预取门控（WiFi 开 = core1 可能在服务网页读 SD）
#include <ESP32epdx.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define TIXML2_PSRAM_POOL          // tinyxml2 池块走 PSRAM（补丁见 VENDOR.md）
#include "tinyxml2/tinyxml2.h"
// 最后 include：Serial 重定向进日志环形缓冲（/debug 网页调试台，DESIGN §11）
#include "DbgLog.h"

using namespace tinyxml2;

// ---- EPUB 临时文件 FS：片内 LittleFS，绝不写 SD --------------------------------
// 坑 18（DESIGN §5.18）：SD 卡写入 ~3KB 即驱动层物理锁死（0x109→读写全 0x107，
// 只能断电重插），而 EPUB 全流程对 SD 只需要读（zip 流 + 字库 + TXT 书）。
// 临时文件改走片内 Flash 的 LittleFS——默认 16M 分区表自带 3.5MB spiffs 分区，
// 零配置改动；最大的 444KB 章节文本也远装得下。
static fs::FS& tempFs() {
  static bool ok = LittleFS.begin(true);   // 首启空分区自动格式化
  if (!ok) Serial.println("WARN: LittleFS temp FS unavailable");
  return LittleFS;
}

// EPUB 临时文件（LittleFS 根目录，__ 前缀避免和书库扫描撞车）
static const char* TEMP_CONTAINER = "/__c.xml";
static const char* TEMP_OPF       = "/__o.opf";
static const char* TEMP_XHTML     = "/__e.xhtml";   // 主核同步转换的 XHTML 临时文件
static const char* TEMP_IMG       = "/__i.bin";   // 插图（解压到渲染同一张时复用）
static const char* TEMP_NCX       = "/__t.ncx";   // 目录源（NCX 或 nav.xhtml 共用）

// ---- 文本汇：DOM 遍历 → LittleFS 临时文本，顺手做空白规范化 ----
// XHTML 源码里缩进/换行全是排版噪音；规则：折叠空白为单空格、块级元素补 '\n'、
// 连续空行封顶 2（省页面行数）。UTF-8 多字节字节值都 ≥0x80，与 ASCII 空白判断无冲突。
// 写出走 2KB 缓冲：逐字节 write 到 LittleFS 会把 280KB 章节拖到几分钟，缓冲后一次刷盘。
class TextSink {
public:
  bool open(fs::FS& fs, const char* path) {
    m_f = fs.open(path, FILE_WRITE);
    m_last = '\n'; m_nl = 1; m_len = 0; m_err = false;
    return (bool)m_f;
  }
  void close() { flush(); if (m_f) m_f.close(); }
  bool err() const { return m_err; }   // true = 有 SD 写失败，文本不完整（调用方判坏章）

  void put(char c) {
    if (m_err) return;                 // 写已停摆：直接丢字符（否则 2KB 一刷 ×5s 连环超时）
    if (c == '\n') {
      if (m_nl >= 2) return;          // 连续空行封顶 2
      m_nl++;
    } else m_nl = 0;
    m_buf[m_len++] = c;
    if (m_len >= (int)sizeof(m_buf)) flush();
    m_last = c;
  }
  void text(const char* s) {
    bool pendingSpace = false;
    for (; *s; s++) {
      char c = *s;
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pendingSpace = true; continue; }
      if (pendingSpace && m_last != '\n' && m_last != ' ') put(' ');
      pendingSpace = false;
      put(c);
    }
  }
  void nl() { if (m_last != '\n') put('\n'); }

  // 块前分隔（emitImageMarker 用）：行首不添任何东西；行内接空格。
  // 不能无脑 nl()——同章相邻两张整页图（漫画对页，见 DESIGN §12）之间
  // 的空行会把右页图挤到下一页，左右屏配对错位。
  void sep() { if (m_last != '\n' && m_last != ' ') put(' '); }

private:
  void flush() {
    if (m_len > 0 && m_f) {
      if (m_f.write((const uint8_t*)m_buf, m_len) != (size_t)m_len) {
        m_err = true;                 // SD 写挂了：置位后 put 全部丢弃，不再刷盘
        Serial.println("WARN: TextSink write failed - stop flushing");
      }
      m_len = 0;
    }
  }
  File m_f;
  char m_buf[2048];
  int m_len;
  char m_last;      // 上一个写入字符
  int m_nl;         // 尾部连续换行数（封顶 2）
  bool m_err;       // 曾有写失败（open 重置）
};

// 块级元素：结束标签后补换行（br 是空元素，进这个表也行——表查在元素处理处）
static bool isBlockTag(const char* n) {
  static const char* tags[] = {
    "p", "div", "h1", "h2", "h3", "h4", "h5", "h6",
    "li", "ul", "ol", "tr", "table", "section",
    "blockquote", "pre", "body", NULL };
  for (int i = 0; tags[i]; i++)
    if (strcmp(n, tags[i]) == 0) return true;
  return false;
}

// ---- EPUB 插图标记：src(href) → zip 内路径 → 写 [[img:path]] 独占一行 ----
// percentDecode 定义在本文件后部，先给原型（emitImageMarker 只在转换路径被调）
static void percentDecode(char* s);
// dirOf 定义在本文件后部（目录解析用），loadChapter/syncConvert 复用先给原型
static void dirOf(const char* zipPath, char* out, size_t n);

// 解析相对 href → 规范化 zip 内路径（'#' 锚截掉、./ 与 ../ 折叠、%XX 解码）。
// 与 emitImageMarker 同一套规则，目录 NCX/nav 的 content src / a href 复用。
static void resolveZipPath(char* out, size_t n, const char* baseDir, const char* src) {
  if (src[0] == '/')
    snprintf(out, n, "%s", src + 1);   // zip 根开头的 '/' 去掉
  else
    snprintf(out, n, "%s%s", baseDir, src);
  char* frag = strchr(out, '#');
  if (frag) *frag = '\0';

  // 折叠 ./ 和 ../：段指针栈，原地把 '/' 换 '\0'
  char* seg[24]; int ns = 0;
  for (char* p = out; *p && ns < 24; ) {
    while (*p == '/') p++;
    if (!*p) break;
    char* start = p;
    while (*p && *p != '/') p++;
    int len = (int)(p - start);
    if (len == 1 && start[0] == '.') continue;
    if (len == 2 && start[0] == '.' && start[1] == '.') { if (ns > 0) ns--; continue; }
    seg[ns++] = start;
    if (*p) *p++ = '\0';
  }
  char path[160] = "";
  for (int i = 0; i < ns; i++) {
    if (path[0]) strncat(path, "/", sizeof(path) - strlen(path) - 1);
    strncat(path, seg[i], sizeof(path) - strlen(path) - 1);
  }
  snprintf(out, n, "%s", path);
  percentDecode(out);
}

// TxtBook 排版时认这行标记、量高占位、回调 EpubBook 解码绘制（白底 1bit 抖动）。
// 路径处理：相对本章 XHTML（docDir 含尾 '/'）→ '../' 折叠（EPUB 图常放兄弟目录，
// 如 OEBPS/text/ch1.xhtml 引 ../images/pic.png）→ '#锚' 截掉 → %XX 解码。
// 路径里空格转 %20：空格会拆词，标记必须作为一个完整"词"过 TxtBook 的词缓冲。
static void emitImageMarker(TextSink& sink, const char* docDir, const char* src) {
  if (strncmp(src, "data:", 5) == 0 || strncmp(src, "http", 4) == 0) return;

  char path[160];
  resolveZipPath(path, sizeof(path), docDir, src);
  if (!path[0]) return;

  sink.sep();
  const char kMarker[] = "[[img:";
  for (const char* p = kMarker; *p; p++) sink.put(*p);
  for (const char* p = path; *p; p++) {
    if (*p == ' ') { sink.put('%'); sink.put('2'); sink.put('0'); }
    else sink.put(*p);
  }
  sink.put(']'); sink.put(']');
  // 后分隔 = 空格而不是换行：同章相邻图标记在文本流里以空格接排，两张整页图
  // 才能连续落满左右面板（换行会先占一个空行，右页图被挤到下一页，漫画对页
  // 配对错位，DESIGN §12）。空格经 put 直写——走 text() 会被 pendingSpace 吞掉。
  // 对文字书：图后的正文接排在本页图下方（placeImageBlock 本就把图独占成块）。
  sink.put(' ');
}

// 遍历 DOM 抽正文。style/script/title/head 子树整棵跳过；body 内才是书。
// <img>/<image>(svg 封面) 不抽文本，转成 [[img:...]] 标记行（图独占一行）。
static void collectText(const XMLNode* node, TextSink& sink, bool inBody,
                        const char* docDir) {
  for (const XMLNode* c = node->FirstChild(); c; c = c->NextSibling()) {
    const XMLText* t = c->ToText();
    if (t) { if (inBody) sink.text(t->Value()); continue; }
    const XMLElement* e = c->ToElement();
    if (!e) continue;                          // 注释/声明/未知节点：跳过
    const char* name = e->Name();
    if (!name) continue;
    if (strcmp(name, "style") == 0 || strcmp(name, "script") == 0) continue;
    if (strcmp(name, "body") == 0) { collectText(e, sink, true, docDir); sink.nl(); continue; }
    if (!inBody) { collectText(e, sink, false, docDir); continue; }   // head/title 等不进正文
    if (strcmp(name, "br") == 0) { sink.nl(); continue; }
    if (strcmp(name, "img") == 0 || strcmp(name, "image") == 0) {
      const char* src = e->Attribute("src");
      if (!src) src = e->Attribute("xlink:href");   // svg <image xlink:href=...>
      if (!src) src = e->Attribute("href");
      if (src) emitImageMarker(sink, docDir, src);
      continue;                                     // 图元素无文本子节点
    }
    collectText(e, sink, true, docDir);
    if (isBlockTag(name)) sink.nl();
  }
}

// ---- 实体预替换（tinyxml2 只认 5 个内置实体，其余原样透传会显示 &nbsp; 原文）----
// 单字符替换表；&#160;/&#xA0; 同 nbsp。压缩式单遍扫描（结果不长于源）。
struct EntMap { const char* name; char repl; };
static const EntMap kEnts[] = {
  {"nbsp", ' '}, {"ldquo", '"'}, {"rdquo", '"'},
  {"lsquo", '\''}, {"rsquo", '\''},
  {"mdash", '-'}, {"ndash", '-'},
  {"times", 'x'}, {"middot", '.'},
};
static void sanitizeEntities(char* buf) {
  char* r = buf;         // 读指针
  char* w = buf;         // 写指针（≤ r，原地压缩）
  while (*r) {
    if (*r != '&') { *w++ = *r++; continue; }
    // 尝试匹配 &name;
    bool done = false;
    for (unsigned i = 0; i < sizeof(kEnts) / sizeof(kEnts[0]); i++) {
      size_t n = strlen(kEnts[i].name);
      if (strncmp(r + 1, kEnts[i].name, n) == 0 && r[n + 1] == ';') {
        *w++ = kEnts[i].repl; r += n + 2; done = true; break;
      }
    }
    if (done) continue;
    if (strncmp(r, "&#160;", 6) == 0 || strncmp(r, "&#xA0;", 6) == 0) {
      *w++ = ' '; r += 6; continue;
    }
    if (strncmp(r, "&hellip;", 8) == 0) {   // … → 三个点（TxtBook 不归一 U+2026）
      memcpy(w, "...", 3); w += 3; r += 8; continue;
    }
    *w++ = *r++;           // 其它（含内置实体 &amp; 等）原样留给 XML 解析器
  }
  *w = '\0';
}

// ---- 兜底：XML 解析失败时的幼稚剥标签（从临时文件分块读，零大内存）----
static bool naiveStripToText(const char* xhtmlPath, const char* txtPath) {
  File in = tempFs().open(xhtmlPath, FILE_READ);
  if (!in) return false;
  TextSink sink;
  if (!sink.open(tempFs(), txtPath)) { in.close(); return false; }
  bool inTag = false;
  while (in.available()) {
    char c = (char)in.read();
    if (inTag) {
      if (c == '>') { inTag = false; sink.nl(); }   // 粗略：每个标签后换行（封顶 2 兜底）
    } else if (c == '<') inTag = true;
    else { char tmp[2] = { c, 0 }; sink.text(tmp); }  // 单字节喂（text 做空白折叠）
  }
  in.close();
  sink.close();
  return !sink.err();          // 兜底路径也看写结果（写停摆时别当成功章）
}

// 读整个临时文件进 PSRAM（+1 给 '\0'）。失败返回 nullptr。
static char* readFileToPsram(fs::FS& fs, const char* path, size_t* outLen) {
  File f = fs.open(path, FILE_READ);
  if (!f) return nullptr;
  size_t n = f.size();
  char* buf = (char*)ps_malloc(n + 1);
  if (!buf) buf = (char*)malloc(n + 1);
  if (!buf) { f.close(); return nullptr; }
  size_t got = f.read((uint8_t*)buf, n);
  f.close();
  buf[got] = '\0';
  if (outLen) *outLen = got;
  return buf;
}

// zip 内路径 %XX 解码（EPUB href 可能带转义；结果不长于源，原地压）
static void percentDecode(char* s) {
  char* r = s; char* w = s;
  while (*r) {
    if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
      char h[3] = { r[1], r[2], 0 };
      *w++ = (char)strtol(h, NULL, 16);
      r += 3;
    } else *w++ = *r++;
  }
  *w = '\0';
}

// ==== 双核章节缓存（DESIGN §13）：core0 预取"转换后的章文本"，翻章 = 开文件 ====
// 背景：loadChapter 同步路径（zip 解压 + XML 解析 + LittleFS 落盘）大章节几秒到
// 十几秒（坑 23），跨章翻页/回退可感知卡顿。S3 的 core0 全程闲着 → 后台任务把
// 当前章前后各一章提前转换进 3 个轮换槽位；主核命中 READY 槽 = 打开文件（毫秒级）。
//
// 槽位状态机（s_mu 保护）：IDLE(0) → WRITING(2) → READY(1)；ch = 槽内章号（-1 无）。
//   主核：acquireSlot 里 READY 命中直用 / 未命中占 IDLE 同步转换；kickAdjacent 占
//         IDLE 或驱逐"离目标章最远"的非当前章 READY 槽（复用即 FILE_WRITE 截断，
//         旧文件内容无关紧要，无需先删）。
//   WRITING 槽绝不驱逐：恰是目标章就等它就绪，否则等它落定再动池子（主核
//         syncConvert 与任务 pfConvert 都会 finalize，无死锁）。
//   当前章槽（m_curSlot，m_txt 正开着它）绝不驱逐/复用。
// 代际 m_gen：open/close 自增 + clearSlots；任务在飞请求经 (gen,slot,ch) 三重校验，
//   陈旧单自然作废（槽位已被清成 IDLE，任务 finalize 时发现"不归我"就放手；其残
//   留的半成品文件由下一次复用 FILE_WRITE 截断覆盖，最多占 3 个槽位体积）。
// SD 串行：s_sdMu 递归互斥。ESP-IDF 4.4 的 SDMMC 卡命令路径没有会话锁，双核并发
//   戳卡会互踩命令 → 所有 zip/SD 访问（含 extractWithRecovery 自救与主核 open
//   全程）持锁；预取任务的解压同理。LittleFS/PSRAM 分配器自身线程安全，不加锁。
// 门控：WiFi 开（core1 可能在服务网页读 SD）或字库流式模式（渲染期也在读 SD）时
//   不发起预取，翻章退回原同步路径（行为同本缓存不存在）。

// 静态成员定义（类内声明见头文件）
SemaphoreHandle_t EpubBook::s_mu = nullptr;
SemaphoreHandle_t EpubBook::s_sdMu = nullptr;
TaskHandle_t EpubBook::s_pfTask = nullptr;
QueueHandle_t EpubBook::s_pfQueue = nullptr;
EpubBook::SlotState EpubBook::s_slot[EpubBook::NSLOT] = {};
EpubBook* EpubBook::s_pfBook = nullptr;

// 3 槽 LittleFS 文本槽位（"__" 前缀避开书库扫描；≤444KB×3 ≈ 1.3MB，3.5MB 分区够）
static const char* const SLOT_PATH[3] = { "/__s0.txt", "/__s1.txt", "/__s2.txt" };
static_assert(EpubBook::NSLOT == 3, "SLOT_PATH 与 NSLOT 同步改");
static const size_t PF_MAX_XHTML = 2 * 1024 * 1024;  // 预取任务 XHTML 进 PSRAM 的上限
                                                     // （实测有书单章 1.2MB，1MB 上限会整章放弃）

// SD 访问锁 RAII（递归互斥：open() 全程持锁时内层 extractWithRecovery 再取不互斥）
// 注意：s_sdMu 是 xSemaphoreCreateRecursiveMutex() 创建的，必须用 TakeRecursive/
// GiveRecursive —— FreeRTOS 文档明确说对递归互斥量用普通 xSemaphoreTake 是未定义行为
struct EpubBook::SdMuGuard {
  SdMuGuard()  { if (s_sdMu) xSemaphoreTakeRecursive(s_sdMu, portMAX_DELAY); }
  ~SdMuGuard() { if (s_sdMu) xSemaphoreGiveRecursive(s_sdMu); }
};

// 章号距离（驱逐挑"最远"用；回卷书 abs 差足够，不做环形距离）
static uint16_t chDist(int16_t a, uint16_t b) {
  int d = (int)a - (int)b;
  return (uint16_t)(d < 0 ? -d : d);
}

void EpubBook::clearSlots() {
  xSemaphoreTake(s_mu, portMAX_DELAY);
  for (int i = 0; i < NSLOT; i++) { s_slot[i].state = 0; s_slot[i].ch = -1; }
  m_curSlot = -1;
  xSemaphoreGive(s_mu);
  for (int i = 0; i < NSLOT; i++) tempFs().remove(SLOT_PATH[i]);
}

void EpubBook::evictSlot(int slot) {
  xSemaphoreTake(s_mu, portMAX_DELAY);
  if (s_slot[slot].state == 1) { s_slot[slot].state = 0; s_slot[slot].ch = -1; }
  xSemaphoreGive(s_mu);
  // 文件不删：下次复用 FILE_WRITE 截断覆盖
}

void EpubBook::invalidateOtherSlots() {
  xSemaphoreTake(s_mu, portMAX_DELAY);
  for (int i = 0; i < NSLOT; i++)
    if (i != m_curSlot && s_slot[i].state == 1) { s_slot[i].state = 0; s_slot[i].ch = -1; }
  xSemaphoreGive(s_mu);
  for (int i = 0; i < NSLOT; i++)
    if (i != m_curSlot) tempFs().remove(SLOT_PATH[i]);
}

// 主核：为第 idx 章拿到一个 READY 槽。命中即用；未命中选槽同步转换（慢路径）。
// 选槽优先级：IDLE > 驱逐最远 READY；WRITING 绝不驱逐——是目标章就等它就绪
// （预取刚好在跑同一章，最常见），不是就等它落定回头重选。
int EpubBook::acquireSlot(uint16_t idx) {
  const uint32_t t0 = millis();
  for (;;) {
    int hit = -1, wTarget = -1, idle = -1, farReady = -1;
    uint16_t farD = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < NSLOT; i++) {
      const int16_t ch = s_slot[i].ch;
      if (s_slot[i].state == 1 && ch == (int16_t)idx) hit = i;
      if (s_slot[i].state == 2 && ch == (int16_t)idx) wTarget = i;
      if (s_slot[i].state == 0 && i != m_curSlot) idle = i;
      if (s_slot[i].state == 1 && i != m_curSlot) {
        uint16_t d = chDist(ch, idx);
        if (farReady < 0 || d > farD) { farReady = i; farD = d; }
      }
    }
    if (hit >= 0) {
      xSemaphoreGive(s_mu);
      if (millis() - t0 > 50)
        Serial.printf("[epub] ch%u cache wait %lu ms\n",
                      (unsigned)idx + 1, (unsigned long)(millis() - t0));
      return hit;
    }
    if (idle >= 0) {
      s_slot[idle].state = 2; s_slot[idle].ch = (int16_t)idx;   // 主核占位 WRITING
      xSemaphoreGive(s_mu);
      bool ok = syncConvert(idx, idle);
      xSemaphoreTake(s_mu, portMAX_DELAY);
      s_slot[idle].state = ok ? 1 : 0;
      if (!ok) s_slot[idle].ch = -1;
      xSemaphoreGive(s_mu);
      return ok ? idle : -1;
    }
    const int wait = (wTarget >= 0) ? wTarget : farReady;  // farReady=-1 → 非当前槽全在写
    xSemaphoreGive(s_mu);
    if (farReady >= 0 && wTarget < 0) { evictSlot(farReady); continue; }
    uint32_t start = millis();
    bool settled = false;
    while (millis() - start < 20000) {
      delay(50);
      xSemaphoreTake(s_mu, portMAX_DELAY);
      int8_t st = s_slot[wait].state;
      xSemaphoreGive(s_mu);
      if (st != 2) { settled = true; break; }
    }
    if (!settled) Serial.println("WARN: slot wait timeout - retrying");
    // 落定（或超时）后回头重选：WRITING→READY 可能正是目标章，池子状态已变
  }
}

// 主核慢路径：解压 + 转换写进指定槽（原 loadChapter 的路径，语义一字未改）
bool EpubBook::syncConvert(uint16_t idx, int slot) {
  if (!extractWithRecovery(m_chapterPath[idx], TEMP_XHTML)) {
    Serial.printf("WARN: extract failed: %s\n", m_chapterPath[idx]);
    return false;
  }
  char docDir[64];
  dirOf(m_chapterPath[idx], docDir, sizeof(docDir));
  bool ok = convertChapterToText(docDir, TEMP_XHTML, SLOT_PATH[slot]);
  tempFs().remove(TEMP_XHTML);
  return ok;
}

// 主核：把当前章前后章的转换请求发给预取任务。无 IDLE 槽时驱逐最远就绪槽
// （持续前进/后退时让缓存始终指着行进方向）。请求丢失/被拒无害：下次 loadChapter
// 会重新 kick。
void EpubBook::kickAdjacent() {
  if (!s_pfTask || m_nChapters < 2) return;
  if (net.wifiOn()) return;    // WiFi 开：core1 可能正在服务网页读 SD——双核戳卡不抢
  if (CJK.sdOpen()) return;    // 字库流式模式：渲染期也在读 SD——同上
  for (int d = -1; d <= 1; d += 2) {
    uint16_t idx = (uint16_t)((m_chapter + m_nChapters + d) % m_nChapters);
    if (idx == m_chapter) continue;
    int slot = -1;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool have = false;
    int idle = -1, farReady = -1;
    uint16_t farD = 0;
    for (int i = 0; i < NSLOT; i++) {
      const int16_t ch = s_slot[i].ch;
      if (s_slot[i].state != 0 && ch == (int16_t)idx) have = true;
      if (s_slot[i].state == 0) idle = i;
      else if (s_slot[i].state == 1 && i != m_curSlot) {
        uint16_t dd = chDist(ch, idx);
        if (farReady < 0 || dd > farD) { farReady = i; farD = dd; }
      }
    }
    if (!have) slot = (idle >= 0) ? idle : farReady;
    if (slot >= 0) { s_slot[slot].state = 2; s_slot[slot].ch = (int16_t)idx; }
    xSemaphoreGive(s_mu);
    if (slot < 0) continue;
    PfReq r = { m_gen, idx, (int8_t)slot };
    if (xQueueSend(s_pfQueue, &r, 0) != pdTRUE) {
      // 队列满（前一单还没消费）：撤回占位，下次转换时自然重试
      xSemaphoreTake(s_mu, portMAX_DELAY);
      if (s_slot[slot].state == 2 && s_slot[slot].ch == (int16_t)idx) {
        s_slot[slot].state = 0; s_slot[slot].ch = -1;
      }
      xSemaphoreGive(s_mu);
    }
  }
}

void EpubBook::pfEnsure() {
  prefetchInit();                    // 幂等：同步原语（mutex/queue）未建则建
  if (s_pfTask) return;              // 任务已在
  // 坑 25（修正）：任务创建必须在 WiFi 连接完成之后。WiFi 连接期间 core0 持跨核
  // 自旋锁跑长临界区，此时向 core0 建任务 = 永久自旋（开机卡死无输出）。
  // kickAdjacent() 已门控 wifiOn()，故此处 WiFi 必关，core0 空闲可建任务。
  if (net.wifiOn()) return;          // 防御：WiFi 开 = core0 不空闲，等下次 kick
  if (xTaskCreatePinnedToCore(pfTask, "epubpf", 8192, nullptr, 1, &s_pfTask, 0)
      != pdPASS) {
    s_pfTask = nullptr;
    Serial.println("WARN: prefetch task create failed - cache disabled");
  }
  s_pfBook = this;
}

void EpubBook::prefetchInit() {
  if (s_mu) return;   // 幂等：同步原语已建
  // 坑 25：必须在 WiFi 启动前（setup 早期）调用。
  // 仅建同步原语（mutex/queue）；core0 任务延迟到 pfEnsure()（kickAdjacent 调用、
  // WiFi 已关时）创建 —— WiFi 连接期间向 core0 建任务会撞 scheduler 自旋锁导致
  // 开机卡死
  s_mu = xSemaphoreCreateMutex();
  s_sdMu = xSemaphoreCreateRecursiveMutex();
  s_pfQueue = xQueueCreate(4, sizeof(PfReq));
  if (!s_mu || !s_sdMu || !s_pfQueue)
    Serial.println("WARN: prefetch sync primitives failed - cache disabled");
}

bool EpubBook::pfMine(int slot, uint32_t gen, uint16_t ch) const {
  if (gen != m_gen) return false;
  bool mine;
  xSemaphoreTake(s_mu, portMAX_DELAY);
  mine = (s_slot[slot].state == 2 && s_slot[slot].ch == (int16_t)ch);
  xSemaphoreGive(s_mu);
  return mine;
}

void EpubBook::pfFinish(int slot, uint32_t gen, uint16_t ch, bool ok) {
  xSemaphoreTake(s_mu, portMAX_DELAY);
  bool mine = (gen == m_gen && s_slot[slot].state == 2 &&
               s_slot[slot].ch == (int16_t)ch);
  if (mine) {
    s_slot[slot].state = ok ? 1 : 0;
    if (!ok) s_slot[slot].ch = -1;
  }
  xSemaphoreGive(s_mu);
  // 不归我（换书/驱逐已完成）：状态已被别人接管，文件由新主人截断覆盖，什么都不做
}

// 预取任务体（core0）：独立 zip 实例 + XHTML 整条目进 PSRAM（免临时文件、不与主核
// 撞名），解析/落盘阶段不碰 SD。任何阶段发现"槽位不归我这单"立即丢弃——期间可能
// 已换书或跳转，半成品文件留给复用方截断。SD 读失败不自救（Storage::recover 是
// 全局操作，留给主核同步路径统一处理），本单作废即可。
bool EpubBook::pfConvert(uint16_t idx, int slot, uint32_t gen) {
  uint32_t t0 = millis();
  char* buf = nullptr;
  size_t blen = 0;
  bool ok = false;
  {
    SdMuGuard sdGuard;
    EpubZip zip;   // miniz 流 per-instance，与主核 m_zip 并发互不干扰
    if (zip.open(m_path))
      ok = zip.extractToBuffer(m_chapterPath[idx], &buf, &blen, PF_MAX_XHTML);
  }
  if (!ok || !buf) { free(buf); return false; }
  if (!pfMine(slot, gen, idx)) { free(buf); return false; }

  char* start = buf;
  if (blen >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB &&
      (uint8_t)buf[2] == 0xBF)
    start += 3;   // 去 UTF-8 BOM
  sanitizeEntities(start);

  ok = false;
  {
    XMLDocument doc;   // 析构在作用域尾：池块（PSRAM）随释放
    if (doc.Parse(start) == XML_SUCCESS) {
      char docDir[64];
      dirOf(m_chapterPath[idx], docDir, sizeof(docDir));
      TextSink sink;
      if (sink.open(tempFs(), SLOT_PATH[slot])) {
        collectText(&doc, sink, false, docDir);
        sink.close();
        ok = !sink.err();
      }
    }
  }
  free(buf);
  if (!ok) return false;
  if (!pfMine(slot, gen, idx)) return false;   // 写盘期间被换书/驱逐：结果作废
  Serial.printf("[epub] prefetch ch%u -> slot%d ok (%lu ms)\n",
                (unsigned)idx + 1, slot, (unsigned long)(millis() - t0));
  return true;
}

void EpubBook::pfTask(void*) {
  PfReq r;
  for (;;) {
    if (xQueueReceive(s_pfQueue, &r, portMAX_DELAY) != pdTRUE) continue;
    EpubBook* self = s_pfBook;
    if (!self) continue;
    if (!self->pfMine(r.slot, r.gen, r.ch)) continue;   // 陈旧单：槽位已易主
    bool ok = self->pfConvert(r.ch, r.slot, r.gen);
    self->pfFinish(r.slot, r.gen, r.ch, ok);
  }
}

bool EpubBook::open(const char* path) {
  // 换书重开：同一本且已开 → 直接复用（槽位缓存同样有效）；不同书 → 全量重置
  static char s_openPath[64] = "";   // 与 LibEntry.path 同宽，长路径截断会误判"同一本"
  if (m_ok && strcmp(s_openPath, path) == 0) return true;
  pfEnsure();                        // 双核预取设施（互斥量/队列/core0 任务）惰性建立
  SdMuGuard sdGuard;                 // open 全程持 SD 锁（含 loadChapter 同步解压）
  m_gen++;                           // 旧书在飞的预取单全部判陈旧
  clearSlots();                      // 槽位文本随上一本书作废
  m_zip.close();
  m_ok = false;
  m_chapterDone = false;
  m_chapter = 0;
  m_nChapters = 0;
  m_nToc = 0;
  m_sizesOk = false;
  m_totalSize = 0;
  m_page = 0;
  m_pairStart = -1;
  m_opfDir[0] = '\0';
  m_title[0] = '\0';
  snprintf(s_openPath, sizeof(s_openPath), "%s", path);
  snprintf(m_path, sizeof(m_path), "%s", path);   // 重挂自救时重开 zip 用
  m_sdDead = false;                               // 新一轮书会话，给 SD 重新判活机会
  m_imgCached = false;                            // 插图临时文件缓存键随书重置

  if (!m_zip.open(path)) { s_openPath[0] = '\0'; return false; }

  // 1) container.xml → rootfile(OPF) 路径
  if (!extractWithRecovery("META-INF/container.xml", TEMP_CONTAINER)) { m_zip.close(); return false; }
  char* cbuf = readFileToPsram(tempFs(), TEMP_CONTAINER, NULL);
  if (!cbuf) { m_zip.close(); return false; }
  const char* opfPath = NULL;
  XMLDocument cdoc;
  if (cdoc.Parse(cbuf) == XML_SUCCESS) {
    if (const XMLElement* cont = cdoc.FirstChildElement("container")) {
      if (const XMLElement* rfs = cont->FirstChildElement("rootfiles")) {
        if (const XMLElement* rf = rfs->FirstChildElement("rootfile")) {
          opfPath = rf->Attribute("full-path");
          if (opfPath && !opfPath[0]) opfPath = NULL;
        }
      }
    }
  }
  if (!opfPath) {
    // 退化：手动找 <rootfile（有的 EPUB 带命名空间或奇怪缩进导致上面失败）
    char* p = strstr(cbuf, "full-path");
    if (p) {
      p = strchr(p, '"'); if (p) {
        char* q = strchr(++p, '"');
        if (q) { *q = '\0'; opfPath = p; }
      }
    }
  }
  char opf[64];
  snprintf(opf, sizeof(opf), "%s", opfPath ? opfPath : "");
  free(cbuf);
  tempFs().remove(TEMP_CONTAINER);
  if (!opf[0]) { Serial.println("WARN: container.xml: no rootfile"); m_zip.close(); return false; }

  // 2) OPF 所在目录（href 拼接用）
  const char* slash = strrchr(opf, '/');
  if (slash) {
    size_t n = slash - opf + 1;
    if (n >= sizeof(m_opfDir)) n = sizeof(m_opfDir) - 1;
    memcpy(m_opfDir, opf, n); m_opfDir[n] = '\0';
  } else m_opfDir[0] = '\0';

  // 3) OPF：标题 + manifest + spine
  if (!parseOpf(opf)) { m_zip.close(); s_openPath[0] = '\0'; return false; }
  if (!m_nChapters) {
    Serial.println("WARN: EPUB spine has no xhtml chapters");
    m_zip.close(); s_openPath[0] = '\0'; return false;
  }

  // 页眉标题：优先 dc:title（parseOpf 已写入 m_title），空则退回文件名
  if (!m_title[0]) {
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(m_title, sizeof(m_title), "%s", base);
  }
  Serial.printf("[epub] %s: %u chapters, title=%s\n",
                path, m_nChapters, m_title);
  m_ok = true;                 // loadChapter 失败不致命：renderPair 会跳坏章
  return loadChapter(0);
}

// 切书/重挂前关句柄（Book::close）。进度状态（m_pairStart/章号/页号）保留，
// 调用方 saveProgress() 切书前照常读得到。
void EpubBook::close() {
  m_gen++;              // 在飞预取判陈旧：finalize 时发现"不归我"即放手
  clearSlots();         // 槽位文本随书关闭作废（含文件删除）
  m_txt.close();
  m_zip.close();
  m_ok = false;
  tempFs().remove(TEMP_IMG);       // 插图临时文件随书关闭作废
  m_imgCached = false; m_imgPath[0] = '\0';
}

// ---- 解压 + SD 失败自救 --------------------------------------------------
// P4 首测（2026-09-28）定案：EPUB 解压的 SD **写**在驱动层停摆 —— 0x107 命令
// 超时 / 0x109 应答 CRC 后卡不再应答，之后每次写固定 ≈5s 超时；而**读全程
// 正常**、全堆完整性每步都过 → 不是 miniz 越界踩内存，是卡/写路径物理挂死
// （只能断电重插，CMD0 重挂救不回）。→ 坑 18：临时文件全部改走片内 LittleFS，
// 本项目对 SD 零写入。
// 本函数保留作 SD **读**（zip 流/字库/TXT 书）出错时的自救，三步走：
//   ① 读探针：连读都死 = 卡彻底挂死 → 只能断电重插，立即放弃（防连环 5s 超时）
//   ② 关全部 SD 句柄 → Storage::recover()（end+begin，卡走 CMD0 重新初始化）
//   ③ 重开 zip 重试解压：成功 = 重挂救得回（固件可自救）；失败 = SD 不可用
bool EpubBook::extractWithRecovery(const char* zipPath, const char* outPath) {
  SdMuGuard sdGuard;   // SD 访问串行（递归：open() 全程持锁时再取不互斥）
  if (m_sdDead) return false;               // 本会话已判死：不再探 SD（防连环超时）

  if (m_zip.extractToFile(zipPath, outPath, tempFs())) return true;
  if (m_zip.lastFail() != EpubZip::EXFAIL_IO) return false;   // 条目缺/校验败：与 SD 无关

  uint32_t t0 = millis();

  // ① 读探针
  bool readOk = false;
  {
    File f = SD_MMC.open(m_path, FILE_READ);
    if (f) {
      uint8_t b[512];
      readOk = (f.read(b, sizeof(b)) == (int)sizeof(b));
      f.close();
    }
  }
  Serial.printf("[sdtriage] read after write-fail: %s (%lu ms)\n",
                readOk ? "OK" : "DEAD", (unsigned long)(millis() - t0));
  if (!readOk) {
    Serial.println("[sdtriage] card wedged - power cycle needed, giving up this session");
    m_sdDead = true;
    return false;
  }
  if (CJK.sdOpen()) {
    // 字体走 SD 流式模式（PSRAM 装不下才发生，N16R8 实测走 PSRAM 整载不会命中）：
    // 流式句柄跨重挂失效，且 CJK.begin() 有 m_ok 短路不会重开 → 重挂会让字画框
    Serial.println("[sdtriage] CJK holds SD handles - skip remount");
    return false;
  }

  // ② 重挂（先关自己手上的句柄；另一本书的句柄由 openBook 切书时关掉）
  m_txt.close();
  m_zip.close();
  if (!Storage::recover() || !m_zip.open(m_path)) {
    Serial.println("[sdtriage] remount/reopen failed - SD unusable");
    m_sdDead = true;
    return false;
  }

  // ③ 重挂后重试（miniz 重新流读 zip 即读探针；写目标是 LittleFS 不受 SD 影响）
  if (m_zip.extractToFile(zipPath, outPath, tempFs())) {
    Serial.printf("[sdtriage] extract OK after remount - recovered (%lu ms total)\n",
                  (unsigned long)(millis() - t0));
    return true;
  }
  Serial.println("[sdtriage] still failing after remount - SD unusable");
  m_sdDead = true;
  return false;
}

// 解析 OPF：dc:title（可无）、manifest(id→href/type)、spine(idref 序列）
bool EpubBook::parseOpf(const char* opfPath) {
  if (!extractWithRecovery(opfPath, TEMP_OPF)) return false;
  char* buf = readFileToPsram(tempFs(), TEMP_OPF, NULL);
  tempFs().remove(TEMP_OPF);
  if (!buf) return false;

  XMLDocument doc;
  if (doc.Parse(buf) != XML_SUCCESS) {
    Serial.printf("WARN: OPF parse error: %s\n", doc.ErrorStr());
    free(buf);
    return false;
  }
  const XMLElement* root = doc.RootElement();   // <package>
  if (!root) { free(buf); return false; }

  // 标题（<metadata><dc:title>）
  if (const XMLElement* md = root->FirstChildElement("metadata")) {
    for (const XMLElement* e = md->FirstChildElement(); e; e = e->NextSiblingElement()) {
      const char* nm = e->Name();
      if (nm && (strcmp(nm, "dc:title") == 0 || strcmp(nm, "title") == 0)) {
        if (e->GetText() && e->GetText()[0])
          snprintf(m_title, sizeof(m_title), "%s", e->GetText());
        break;
      }
    }
  }

  // manifest：href 按 id 暂存（id ≤31B，href ≤63B，章书足够）
  struct Man { char id[32]; char href[64]; bool html; bool ncx; bool nav; };
  static Man man[128];
  int nman = 0;
  if (const XMLElement* mf = root->FirstChildElement("manifest")) {
    for (const XMLElement* it = mf->FirstChildElement("item"); it && nman < 128;
         it = it->NextSiblingElement("item")) {
      const char* id = it->Attribute("id");
      const char* href = it->Attribute("href");
      const char* mt = it->Attribute("media-type");
      if (!id || !href || !mt) continue;
      snprintf(man[nman].id, sizeof(man[nman].id), "%s", id);
      snprintf(man[nman].href, sizeof(man[nman].href), "%s", href);
      man[nman].html = (strstr(mt, "xhtml") != NULL || strstr(mt, "html") != NULL);
      man[nman].ncx = (strstr(mt, "dtbncx") != NULL);   // EPUB2 目录
      const char* props = it->Attribute("properties");
      man[nman].nav = (props && strstr(props, "nav") != NULL);  // EPUB3 目录
      nman++;
    }
  }

  // spine：idref → manifest → 章路径（拼接 opf 目录 + 解码）
  m_nChapters = 0;
  if (const XMLElement* sp = root->FirstChildElement("spine")) {
    for (const XMLElement* ir = sp->FirstChildElement("itemref"); ir && m_nChapters < MAXCH;
         ir = ir->NextSiblingElement("itemref")) {
      const char* idref = ir->Attribute("idref");
      if (!idref) continue;
      for (int i = 0; i < nman; i++) {
        if (strcmp(man[i].id, idref) == 0 && man[i].html) {
          char* dst = m_chapterPath[m_nChapters];
          snprintf(dst, 64, "%s%s", m_opfDir, man[i].href);
          char* frag = strchr(dst, '#');         // 理论上没有，防一手
          if (frag) *frag = '\0';
          percentDecode(dst);
          m_nChapters++;
          break;
        }
      }
    }
  }

  // 目录：EPUB2 用 NCX（media-type dtbncx），EPUB3 用 nav.xhtml（properties=nav）；
  // 都没有 → m_nToc 保持 0，tocCount/tocTitle 退化"第N章"合成目录。
  m_nToc = 0;
  for (int i = 0; i < nman; i++) {
    if (!man[i].ncx) continue;
    if (parseNcxToc(man[i].href, m_opfDir)) break;
  }
  if (!m_nToc) {
    for (int i = 0; i < nman; i++) {
      if (!man[i].nav) continue;
      if (parseNavToc(man[i].href, m_opfDir)) break;
    }
  }
  if (m_nToc) Serial.printf("[epub] toc: %u entries\n", (unsigned)m_nToc);

  free(buf);
  return true;
}

// XHTML 临时文件 → 纯文本临时文件（tinyxml2 为主，剥标签兜底）。
// docDir = 本章 XHTML 所在 zip 内目录（插图 href 相对它解析，含尾 '/'）
// 主核同步路径与 core0 预取任务共用（路径参数化，互不撞文件）。
bool EpubBook::convertChapterToText(const char* docDir, const char* xhtmlPath,
                                    const char* txtPath) {
  File xf = tempFs().open(xhtmlPath, FILE_READ);
  if (!xf) return false;
  size_t xsize = xf.size();
  xf.close();

  char* buf = readFileToPsram(tempFs(), xhtmlPath, NULL);
  if (!buf) {
    Serial.println("WARN: chapter too big for RAM - naive strip fallback");
    return naiveStripToText(xhtmlPath, txtPath);
  }
  // 去 UTF-8 BOM
  char* start = buf;
  if ((uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF)
    start += 3;
  sanitizeEntities(start);

  uint32_t t0 = millis();
  bool ok = false;
  {
    XMLDocument doc;                    // 析构在作用域尾：池块（PSRAM）随释放
    if (doc.Parse(start) == XML_SUCCESS) {
      TextSink sink;
      if (sink.open(tempFs(), txtPath)) {
        collectText(&doc, sink, false, docDir);
        sink.close();
        ok = !sink.err();          // 临时文件写停摆 → 文本不完整 → 判坏章
        if (!ok) Serial.println("WARN: chapter text write failed");
      }
    } else {
      Serial.printf("WARN: XHTML parse failed (%s) - naive strip fallback\n",
                    doc.ErrorStr());
    }
  }
  free(buf);
  if (!ok) return naiveStripToText(xhtmlPath, txtPath);

  File tf = tempFs().open(txtPath, FILE_READ);
  size_t tsize = tf ? tf.size() : 0;
  if (tf) tf.close();
  Serial.printf("[epub] chapter: xhtml %uB -> text %uB, %lu ms | psram free=%u\n",
                (unsigned)xsize, (unsigned)tsize,
                (unsigned long)(millis() - t0), (unsigned)ESP.getFreePsram());
  return tsize > 0;
}

// 解压一章 + 转文本 + 交给 TxtBook。false = 本章坏（调用方跳过）。
// 双核缓存（DESIGN §13）：槽位 READY 命中 = 打开文件（毫秒级，跨章翻页/回退的
// 常见路径）；未命中由 acquireSlot 走同步解压转换（原慢路径，几秒到十几秒）。
bool EpubBook::loadChapter(uint16_t idx) {
  if (idx >= m_nChapters) return false;
  m_txt.close();   // 先关旧句柄：槽位文件马上被新章占用/复用（FILE_WRITE 截断），
                   // 两处同开一个文件不干净；转换失败时旧章文本也别留在渲染路径上

  const uint32_t t0 = millis();
  int slot = acquireSlot(idx);
  if (slot < 0) return false;

  m_txt.setQuiet(true);            // 章节切换频繁，头部 dump 会刷屏（须在 open 前）
  if (!m_txt.open(tempFs(), SLOT_PATH[slot])) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_slot[slot].state == 1 && s_slot[slot].ch == (int16_t)idx) {
      s_slot[slot].state = 0; s_slot[slot].ch = -1;   // 槽文件打不开：视为无效
    }
    xSemaphoreGive(s_mu);
    return false;
  }
  m_curSlot = slot;
  Serial.printf("[epub] load ch%u slot%d (%lu ms)\n",
                (unsigned)idx + 1, slot, (unsigned long)(millis() - t0));
  m_txt.setLoop(false);            // 章末停住，由本类推进下一章
  m_txt.setTier(m_tier);
  m_txt.setFamily(m_family);
  m_txt.setImageFns(measTramp, drawTramp, this);   // 图标记 → 本类解码绘制
  char hdr[40];
  snprintf(hdr, sizeof(hdr), "%s ch%u", m_title, (unsigned)idx + 1);
  m_txt.setTitle(hdr);
  m_chapter = idx;
  m_chapterDone = false;
  kickAdjacent();      // 预取前后章（core0 空闲时跑；命中后下一次翻章 = 开文件）
  return true;
}

// ---- 插图解码回调（TxtBook 排版到 [[img:...]] 标记时经 trampoline 进来）----
// 流程：zip 条目 →（同路径缓存跳过重复解压）→ LittleFS 临时文件 → EpubImg 探尺寸/解码
bool EpubBook::ensureImageTmp(const char* zipPath) {
  if (m_imgCached && strcmp(m_imgPath, zipPath) == 0) return m_imgOk;
  m_imgCached = true;
  snprintf(m_imgPath, sizeof(m_imgPath), "%s", zipPath);
  tempFs().remove(TEMP_IMG);
  m_imgOk = extractWithRecovery(zipPath, TEMP_IMG);
  return m_imgOk;
}

bool EpubBook::measureImage(const char* zipPath, int maxW, int* outH) {
  if (!ensureImageTmp(zipPath)) return false;
  int iw = 0, ih = 0;
  if (!EpubImg::probe(TEMP_IMG, &iw, &ih)) return false;   // 不支持的格式/坏图
  *outH = iw >= maxW ? (int)((int64_t)ih * maxW / iw) : ih;  // 宽贴满的自然高（小图不放大）
  return true;
}

bool EpubBook::drawImage(const char* zipPath, int x, int y, int maxW, int maxH) {
  if (!ensureImageTmp(zipPath)) return false;
  uint32_t t0 = millis();
  bool ok = EpubImg::draw(TEMP_IMG, x, y, maxW, maxH);
  Serial.printf("[epub] image %s: %s (%lu ms)\n", zipPath, ok ? "ok" : "FAIL",
                (unsigned long)(millis() - t0));
  return ok;
}

bool EpubBook::nextChapter() {
  for (uint16_t guard = 0; guard < m_nChapters; guard++) {
    uint16_t idx = (uint16_t)((m_chapter + 1) % m_nChapters);
    if (loadChapter(idx)) return true;
    m_chapter = idx;                 // 坏章也要推进，防死循环
    Serial.printf("WARN: skip broken chapter %u\n", (unsigned)idx + 1);
  }
  return false;                      // 全是坏章
}

void EpubBook::setTier(uint8_t sizePx) {
  m_tier = sizePx;
  if (m_txt.isOpen()) m_txt.setTier(sizePx);
}

bool EpubBook::setFamily(const char* name) {
  snprintf(m_family, sizeof(m_family), "%s", name ? name : "classic");
  return m_txt.isOpen() ? m_txt.setFamily(m_family) : true;
}

bool EpubBook::renderPair(uint8_t* fbL, uint8_t* fbR) {
  if (!m_ok) return false;

  if (m_chapterDone) {               // 上一章翻完：进下一章（读完回卷第一章）
    if (!nextChapter()) { m_ok = false; return false; }
  }

  // 记录对页起点（restartPair / 进度存档）：章号 + 章内位置 + 页号
  m_pairStartCh = m_chapter;
  m_pairStart = m_txt.curFilePos();
  m_pairStartPage = m_page;
  m_txt.setPermille((int)permille());   // 页眉进度% = 全局（章权重+章内偏移）

  // 左页。空章（转换失败/无文本/纯空白页）连续跳过，全坏则书不可用。
  // 纯空白页 = TxtBook 按"无内容"返回 false（见 TxtBook::renderPage 的
  // s_hadContent 约定）；漫画章尾残留的 "\n\n" 不再渲成"有页码的空白页"。
  // guard 封顶：整书全是空白章时绝不死循环。
  bool okL = m_txt.renderPage(fbL, m_page);
  for (uint16_t guard = 0; !okL && guard < m_nChapters; guard++) {
    Serial.printf("[epub] chapter %u exhausted/empty, advancing\n", (unsigned)m_chapter + 1);
    if (!nextChapter()) { m_ok = false; return false; }
    // 对页起点随章更新（进度存档/restartPair 要落在真正渲染的章上）
    m_pairStartCh = m_chapter;
    m_pairStart = m_txt.curFilePos();
    okL = m_txt.renderPage(fbL, m_page);
  }
  if (!okL) { m_ok = false; return false; }

  // 右页：章末中道结束 → 空白页 + 标记下一对页进新章
  if (m_txt.hitEof()) {
    m_txt.renderBlank(fbR, m_page + 1);
    m_chapterDone = true;
  } else {
    bool okR = m_txt.renderPage(fbR, m_page + 1);
    if (!okR) m_txt.renderBlank(fbR, m_page + 1);   // 右页起点的章末（极少见）
    if (m_txt.hitEof()) m_chapterDone = true;
  }
  m_page += 2;
  return true;
}

bool EpubBook::restartPair() {
  if (!m_ok || m_pairStart < 0) return false;
  if (m_pairStartCh != m_chapter) {
    if (!loadChapter(m_pairStartCh)) return false;
    invalidateOtherSlots();   // 跨章回退同 jump：邻章缓存指向旧位置，清掉重填
    kickAdjacent();
  }
  m_chapterDone = false;
  m_page = m_pairStartPage;
  return m_txt.restorePair(m_pairStart, m_pairStartPage);
}

bool EpubBook::restorePair(uint16_t ch, long off, uint32_t page) {
  if (!m_ok) return false;
  if (ch >= m_nChapters) {
    // 章号非法：退回第一章开头
    if (!loadChapter(0)) return false;
    off = 0; page = 0;
  } else if (ch != m_chapter && !loadChapter(ch)) {
    // 跨章回退才需重新解压转换；同章（回退最常见情形）m_txt 的临时文本仍
    // 对应当前章，直接 seek 即可——无条件 loadChapter 会把每次同章回退都
    // 拖成整章解压+解析（几秒到十几秒，TXT 翻页快即因它只 seek）
    if (!loadChapter(0)) return false;
    off = 0; page = 0;
  }
  m_chapterDone = false;
  m_page = page;
  m_pairStart = off;
  m_pairStartPage = page;
  m_pairStartCh = m_chapter;
  return m_txt.restorePair(off, page);
}

// ==== 阅读功能（2026-10-12：目录/进度%/跳转）================================

int EpubBook::chapterIndexOf(const char* zipPath) const {
  for (uint16_t i = 0; i < m_nChapters; i++)
    if (strcmp(m_chapterPath[i], zipPath) == 0) return (int)i;
  return -1;
}

void EpubBook::addToc(uint16_t ch, const char* title) {
  if (m_nToc >= MAXCH || ch >= m_nChapters) return;
  if (!m_tocBuf) {                       // 惰性分配（PSRAM 12KB，失败则不用真实目录）
    m_tocBuf = (char*)ps_malloc((size_t)MAXCH * 48);
    if (!m_tocBuf) { Serial.println("WARN: toc buffer alloc failed"); return; }
  }
  m_tocCh[m_nToc] = ch;
  snprintf(m_tocBuf + (size_t)m_nToc * 48, 48, "%s", title ? title : "");
  m_nToc++;
}

// 目录源路径的基目录（含尾 '/'；无目录 → 空串 = zip 根）
static void dirOf(const char* zipPath, char* out, size_t n) {
  const char* slash = strrchr(zipPath, '/');
  if (!slash) { out[0] = '\0'; return; }
  size_t len = (size_t)(slash - zipPath) + 1;
  if (len >= n) len = n - 1;
  memcpy(out, zipPath, len);
  out[len] = '\0';
}

bool EpubBook::parseNcxToc(const char* href, const char* opfDir) {
  char zp[96];
  resolveZipPath(zp, sizeof(zp), opfDir, href);
  if (!extractWithRecovery(zp, TEMP_NCX)) return false;
  char* buf = readFileToPsram(tempFs(), TEMP_NCX, NULL);
  tempFs().remove(TEMP_NCX);
  if (!buf) return false;
  XMLDocument doc;
  if (doc.Parse(buf) != XML_SUCCESS) { free(buf); return false; }
  const XMLElement* nm = doc.FirstChildElement("ncx");
  if (!nm) nm = doc.RootElement();
  char base[64];
  dirOf(zp, base, sizeof(base));
  if (nm) {
    if (const XMLElement* nmap = nm->FirstChildElement("navMap"))
      walkNcx(nmap, base);
  }
  free(buf);
  return m_nToc > 0;
}

void EpubBook::walkNcx(const tinyxml2::XMLElement* np, const char* baseDir) {
  for (const XMLElement* c = np->FirstChildElement("navPoint"); c;
       c = c->NextSiblingElement("navPoint")) {
    char tbuf[48] = "";
    if (const XMLElement* nl = c->FirstChildElement("navLabel"))
      if (const XMLElement* tx = nl->FirstChildElement("text"))
        if (tx->GetText()) snprintf(tbuf, sizeof(tbuf), "%s", tx->GetText());
    if (const XMLElement* ct = c->FirstChildElement("content")) {
      const char* src = ct->Attribute("src");
      if (src) {
        char zp[160];
        resolveZipPath(zp, sizeof(zp), baseDir, src);
        int ch = chapterIndexOf(zp);
        if (ch >= 0) addToc((uint16_t)ch, tbuf);
      }
    }
    walkNcx(c, baseDir);   // 嵌套 navPoint（子章节）
  }
}

bool EpubBook::parseNavToc(const char* href, const char* opfDir) {
  char zp[96];
  resolveZipPath(zp, sizeof(zp), opfDir, href);
  if (!extractWithRecovery(zp, TEMP_NCX)) return false;
  char* buf = readFileToPsram(tempFs(), TEMP_NCX, NULL);
  tempFs().remove(TEMP_NCX);
  if (!buf) return false;
  XMLDocument doc;
  if (doc.Parse(buf) != XML_SUCCESS) { free(buf); return false; }
  char base[64];
  dirOf(zp, base, sizeof(base));
  const XMLElement* root = doc.RootElement();
  // 找目录 nav：epub:type="toc"；没有则退第一个含 <ol> 的 <nav>
  const XMLElement* nav = nullptr;
  for (const XMLElement* e = root ? root->FirstChildElement() : nullptr; e;
       e = e->NextSiblingElement()) {
    if (strcmp(e->Name(), "nav") != 0) continue;
    const char* t = e->Attribute("epub:type");
    if (t && strcmp(t, "toc") == 0) { nav = e; break; }
    if (!nav && e->FirstChildElement("ol")) nav = e;
  }
  // body 里再兜一层（nav 可能在 body 下，上面只扫了顶层兄弟）
  if (!nav && root) {
    for (const XMLElement* e = root->FirstChildElement(); e && !nav;
         e = e->NextSiblingElement()) {
      if (strcmp(e->Name(), "body") != 0) continue;
      for (const XMLElement* b = e->FirstChildElement(); b; b = b->NextSiblingElement()) {
        if (strcmp(b->Name(), "nav") != 0) continue;
        const char* t = b->Attribute("epub:type");
        if ((t && strcmp(t, "toc") == 0) || b->FirstChildElement("ol")) { nav = b; break; }
      }
    }
  }
  if (nav) {
    if (const XMLElement* ol = nav->FirstChildElement("ol")) walkNav(ol, base);
  }
  free(buf);
  return m_nToc > 0;
}

void EpubBook::walkNav(const tinyxml2::XMLElement* ol, const char* baseDir) {
  for (const XMLElement* li = ol->FirstChildElement("li"); li;
       li = li->NextSiblingElement("li")) {
    if (const XMLElement* a = li->FirstChildElement("a")) {
      const char* href = a->Attribute("href");
      if (href) {
        char zp[160];
        resolveZipPath(zp, sizeof(zp), baseDir, href);
        int ch = chapterIndexOf(zp);
        if (ch >= 0) addToc((uint16_t)ch, a->GetText());
      }
    }
    if (const XMLElement* sub = li->FirstChildElement("ol")) walkNav(sub, baseDir);
  }
}

bool EpubBook::tocTitle(int idx, char* out, int n) {
  if (!out || n <= 0 || idx < 0 || idx >= tocCount()) return false;
  if (m_nToc) {
    snprintf(out, n, "%s", m_tocBuf + (size_t)idx * 48);
    if (!out[0]) snprintf(out, n, "第%u章", (unsigned)m_tocCh[idx] + 1);  // 空标题兜底
  } else {
    snprintf(out, n, "第%u章", (unsigned)idx + 1);   // 合成目录（无 NCX/nav 的书）
  }
  return true;
}

// 章权重一次建表：O(N) 遍历中央目录（file_stat 按索引），章路径命中 spine 记权重。
// 权重 = 章 XHTML 解压尺寸（∝ 文本量），进度%与按%跳转选章都用它。
void EpubBook::chSizeCb(const char* path, uint32_t size, void* ctx) {
  EpubBook* self = (EpubBook*)ctx;
  int ch = self->chapterIndexOf(path);
  if (ch >= 0 && self->m_chSize[ch] == 0) {
    self->m_chSize[ch] = size;
    self->m_totalSize += size;
  }
}

bool EpubBook::ensureChSizes() {
  if (m_sizesOk) return true;
  memset(m_chSize, 0, sizeof(m_chSize));
  m_totalSize = 0;
  { SdMuGuard g; m_zip.forEach(chSizeCb, this); }   // SD 访问串行（预取任务并发）
  m_sizesOk = true;
  return m_totalSize > 0;
}

// 全书进度千分比 = (前面章权重和 + 本章权重 × 章内偏移/章文本大小) / 总权重。
// 章内偏移→文本比例换算用本章临时文本大小（XHTML 尺寸≠文本尺寸，比例换算抵消）。
uint16_t EpubBook::permille() {
  if (!m_ok) return 0xFFFF;
  if (!ensureChSizes() || !m_totalSize) return 0xFFFF;
  uint32_t cum = 0;
  for (uint16_t i = 0; i < m_chapter && i < m_nChapters; i++) cum += m_chSize[i];
  uint64_t numer = (uint64_t)cum * 1000;
  uint32_t ts = (uint32_t)m_txt.textSize();
  uint32_t cs = m_chSize[m_chapter];
  if (cs && ts > 1 && m_pairStart > 0)
    numer += (uint64_t)m_pairStart * 1000 * cs / ts;
  uint32_t p = (uint32_t)(numer / m_totalSize);
  return p > 1000 ? 1000 : (uint16_t)p;
}

// 全书页数估计：总文本量（权重 × 当前章 文本/权重 比例）/ 平均文本页字节。
// 页号只是显示参考（% 是精确的），跳转后页眉 p.N 为估计值。
uint32_t EpubBook::estTotalPages() {
  ensureChSizes();
  uint32_t num, den;
  m_txt.avgBytes(&num, &den);
  if (!num || !den || !m_totalSize) return (uint32_t)m_nChapters * 10;
  uint32_t ts = (uint32_t)m_txt.textSize();
  uint32_t cs = m_chSize[m_chapter];
  uint64_t totalText = (cs && ts) ? (uint64_t)m_totalSize * ts / cs : m_totalSize;
  uint32_t tp = (uint32_t)(totalText * den / num);
  return tp ? tp : 1;
}

bool EpubBook::jumpPermille(uint16_t p) {
  if (!m_ok) return false;
  if (!ensureChSizes() || !m_totalSize) return false;
  uint64_t target = (uint64_t)p * m_totalSize / 1000;
  uint32_t cum = 0;
  uint16_t ch = m_nChapters - 1;
  for (uint16_t i = 0; i < m_nChapters; i++) {
    if (cum + m_chSize[i] > target || i == m_nChapters - 1) { ch = i; break; }
    cum += m_chSize[i];
  }
  if (!loadChapter(ch)) return false;
  invalidateOtherSlots();   // 旧位置的邻章缓存对新位置无意义：清掉让 kick 重填
  kickAdjacent();
  m_chapterDone = false;
  long ts = m_txt.textSize();
  long off = 0;
  if (m_chSize[ch] && ts > 1) {          // 章内定位：目标在章权重区间内的比例 → 文本偏移
    uint64_t into = target - cum;
    off = (long)(into * (uint64_t)ts / m_chSize[ch]);
    if (off >= ts) off = ts - 1;
  }
  if (!m_txt.seekTo(off)) return false;
  m_page = (uint32_t)((uint64_t)p * estTotalPages() / 1000) & ~1u;
  m_pairStart = m_txt.curFilePos();
  m_pairStartPage = m_page;
  m_pairStartCh = m_chapter;
  m_txt.setPermille((int)permille());
  Serial.printf("[epub] jump to %u%% -> ch%u off %ld\n", (unsigned)p / 10,
                (unsigned)m_chapter + 1, m_pairStart);
  return true;
}

bool EpubBook::jumpToToc(int idx) {
  if (!m_ok) return false;
  uint16_t ch;
  if (m_nToc) {
    if (idx < 0 || idx >= m_nToc) return false;
    ch = m_tocCh[idx];
  } else {
    if (idx < 0 || idx >= m_nChapters) return false;
    ch = (uint16_t)idx;                  // 合成目录 = 章号直跳
  }
  if (ch >= m_nChapters || !loadChapter(ch)) return false;
  invalidateOtherSlots();   // 同 jumpPermille：旧邻章缓存作废，kick 重填新邻章
  kickAdjacent();
  m_chapterDone = false;
  // 页号估计：章首的进度% × 全书估计页数
  ensureChSizes();
  uint16_t p = 0;
  if (m_totalSize) {
    uint32_t cum = 0;
    for (uint16_t i = 0; i < ch; i++) cum += m_chSize[i];
    p = (uint16_t)((uint64_t)cum * 1000 / m_totalSize);
  }
  m_page = (uint32_t)((uint64_t)p * estTotalPages() / 1000) & ~1u;
  m_txt.restorePair(0, m_page);
  m_pairStart = 0;
  m_pairStartPage = m_page;
  m_pairStartCh = m_chapter;
  m_txt.setPermille((int)permille());
  return true;
}
