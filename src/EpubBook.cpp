#include "EpubBook.h"
#include "Storage.h"
#include "SD_MMC.h"
#include "EinkPanel.h"
#include "CjkFont.h"
#include <ESP32epdx.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define TIXML2_PSRAM_POOL          // tinyxml2 池块走 PSRAM（补丁见 VENDOR.md）
#include "tinyxml2/tinyxml2.h"

using namespace tinyxml2;

// ===== 临时诊断（P4 崩坏定位，定案后删）=====
// 每阶段后做一次全堆完整性校验：poisoning 开着时，坏块头一写出来就能被抓到，
// 比等 free 时 abort 早得多 —— 用来定位是哪个阶段踩了 PSRAM 堆。
#include <esp_heap_caps.h>
static void chk(const char* tag) {
  bool ok = heap_caps_check_integrity_all(true);
  Serial.printf("[heap %s] %s | psram free=%u heap free=%u\n",
                ok ? "ok " : "BROKEN", tag,
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());
}
// ===== 诊断结束 =====

// SD 临时文件（根目录，__ 前缀避免和书库扫描撞车）
static const char* TEMP_CONTAINER = "/__c.xml";
static const char* TEMP_OPF       = "/__o.opf";
static const char* TEMP_XHTML     = "/__e.xhtml";
static const char* TEMP_TXT       = "/__e.txt";

// ---- 文本汇：DOM 遍历 → SD 临时文本，顺手做空白规范化 ----
// XHTML 源码里缩进/换行全是排版噪音；规则：折叠空白为单空格、块级元素补 '\n'、
// 连续空行封顶 2（省页面行数）。UTF-8 多字节字节值都 ≥0x80，与 ASCII 空白判断无冲突。
// 写出走 2KB 缓冲：逐字节 write 到 SD 会把 280KB 章节拖到几分钟，缓冲后一次刷盘。
class TextSink {
public:
  bool open(const char* path) {
    m_f = SD_MMC.open(path, FILE_WRITE);
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

// 遍历 DOM 抽正文。style/script/title/head 子树整棵跳过；body 内才是书。
static void collectText(const XMLNode* node, TextSink& sink, bool inBody) {
  for (const XMLNode* c = node->FirstChild(); c; c = c->NextSibling()) {
    const XMLText* t = c->ToText();
    if (t) { if (inBody) sink.text(t->Value()); continue; }
    const XMLElement* e = c->ToElement();
    if (!e) continue;                          // 注释/声明/未知节点：跳过
    const char* name = e->Name();
    if (!name) continue;
    if (strcmp(name, "style") == 0 || strcmp(name, "script") == 0) continue;
    if (strcmp(name, "body") == 0) { collectText(e, sink, true); sink.nl(); continue; }
    if (!inBody) { collectText(e, sink, false); continue; }   // head/title 等不进正文
    if (strcmp(name, "br") == 0) { sink.nl(); continue; }
    collectText(e, sink, true);
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

// ---- 兜底：XML 解析失败时的幼稚剥标签（从 SD 临时文件分块读，零大内存）----
static bool naiveStripToText(const char* xhtmlPath, const char* txtPath) {
  File in = SD_MMC.open(xhtmlPath, FILE_READ);
  if (!in) return false;
  TextSink sink;
  if (!sink.open(txtPath)) { in.close(); return false; }
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
  return !sink.err();          // 兜底路径也看写结果（SD 停摆时别当成功章）
}

// 读整个 SD 文件进 PSRAM（+1 给 '\0'）。失败返回 nullptr。
static char* readFileToPsram(const char* path, size_t* outLen) {
  File f = SD_MMC.open(path, FILE_READ);
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

bool EpubBook::open(const char* path) {
  // 换书重开：同一本且已开 → 直接复用；不同书 → 全量重置
  static char s_openPath[64] = "";   // 与 LibEntry.path 同宽，长路径截断会误判"同一本"
  if (m_ok && strcmp(s_openPath, path) == 0) return true;
  m_zip.close();
  m_ok = false;
  m_chapterDone = false;
  m_chapter = 0;
  m_nChapters = 0;
  m_page = 0;
  m_pairStart = -1;
  m_opfDir[0] = '\0';
  m_title[0] = '\0';
  snprintf(s_openPath, sizeof(s_openPath), "%s", path);
  snprintf(m_path, sizeof(m_path), "%s", path);   // 重挂自救时重开 zip 用
  m_sdDead = false;                               // 新一轮书会话，给 SD 重新判活机会

  if (!m_zip.open(path)) { s_openPath[0] = '\0'; return false; }
  chk("zip.open");

  // 1) container.xml → rootfile(OPF) 路径
  if (!extractWithRecovery("META-INF/container.xml", TEMP_CONTAINER)) { m_zip.close(); return false; }
  chk("container.extract");
  char* cbuf = readFileToPsram(TEMP_CONTAINER, NULL);
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
  SD_MMC.remove(TEMP_CONTAINER);
  chk("container.parse");
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
  chk("opf.parse");
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
  m_txt.close();
  m_zip.close();
  m_ok = false;
}

// ---- 解压 + SD 写失败自救 --------------------------------------------------
// P4 首测（2026-09-28）：EPUB 解压的 SD 写在驱动层停摆 —— 0x107 命令超时 /
// 0x109 应答 CRC 后卡不再应答，之后每次写固定 ≈5s 超时；而**读全程正常**、
// 全堆完整性每步都过 → 不是 miniz 越界踩内存，是卡/写路径在驱动层挂了。
// 三步走，日志带耗时（下一轮串口日志即可定案）：
//   ① 读探针：连读都死 = 卡彻底挂死 → 只能断电重插，立即放弃（防连环 5s 超时）
//   ② 关全部 SD 句柄 → Storage::recover()（end+begin，卡走 CMD0 重新初始化）
//   ③ 重开 zip 重试解压：成功 = 重挂救得回（固件可自救）；失败 = SD 不可用
bool EpubBook::extractWithRecovery(const char* zipPath, const char* outPath) {
  if (m_sdDead) return false;               // 本会话已判死：跳过一切 SD 写

  if (m_zip.extractToFile(zipPath, outPath)) return true;
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

  // ③ 重挂后重试（这次的写就是写探针）
  if (m_zip.extractToFile(zipPath, outPath)) {
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
  char* buf = readFileToPsram(TEMP_OPF, NULL);
  SD_MMC.remove(TEMP_OPF);
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
  struct Man { char id[32]; char href[64]; bool html; };
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
  free(buf);
  return true;
}

// XHTML 临时文件 → 纯文本临时文件（tinyxml2 为主，剥标签兜底）
bool EpubBook::convertChapterToText() {
  File xf = SD_MMC.open(TEMP_XHTML, FILE_READ);
  if (!xf) return false;
  size_t xsize = xf.size();
  xf.close();

  char* buf = readFileToPsram(TEMP_XHTML, NULL);
  if (!buf) {
    Serial.println("WARN: chapter too big for RAM - naive strip fallback");
    return naiveStripToText(TEMP_XHTML, TEMP_TXT);
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
      if (sink.open(TEMP_TXT)) {
        collectText(&doc, sink, false);
        sink.close();
        ok = !sink.err();          // SD 写停摆 → 文本不完整 → 判坏章
        if (!ok) Serial.println("WARN: chapter text write failed");
      }
    } else {
      Serial.printf("WARN: XHTML parse failed (%s) - naive strip fallback\n",
                    doc.ErrorStr());
    }
  }
  free(buf);
  chk("xml.parse+sink");
  if (!ok) return naiveStripToText(TEMP_XHTML, TEMP_TXT);

  File tf = SD_MMC.open(TEMP_TXT, FILE_READ);
  size_t tsize = tf ? tf.size() : 0;
  if (tf) tf.close();
  Serial.printf("[epub] chapter %u: xhtml %uB -> text %uB, %lu ms | psram free=%u\n",
                m_chapter, (unsigned)xsize, (unsigned)tsize,
                (unsigned long)(millis() - t0), (unsigned)ESP.getFreePsram());
  return tsize > 0;
}

// 解压一章 + 转文本 + 交给 TxtBook。false = 本章坏（调用方跳过）
bool EpubBook::loadChapter(uint16_t idx) {
  if (idx >= m_nChapters) return false;
  Serial.printf("[dbg] loadChapter(%u) enter\n", (unsigned)idx);
  extractWithRecovery(m_chapterPath[idx], TEMP_XHTML) ||
      Serial.printf("WARN: extract failed: %s\n", m_chapterPath[idx]);
  chk("extract");
  bool ok = convertChapterToText();
  chk("convert");
  SD_MMC.remove(TEMP_XHTML);
  if (!ok) return false;

  if (!m_txt.open(TEMP_TXT)) return false;
  chk("txt.open");
  m_txt.setQuiet(true);            // 章节切换频繁，头部 dump 会刷屏
  m_txt.setLoop(false);            // 章末停住，由本类推进下一章
  m_txt.setTier(m_tier);
  m_txt.setFamily(m_family);
  char hdr[40];
  snprintf(hdr, sizeof(hdr), "%s ch%u", m_title, (unsigned)idx + 1);
  m_txt.setTitle(hdr);
  m_chapter = idx;
  m_chapterDone = false;
  return true;
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

  // 左页。空章（转换失败/无文本）连续跳过，全坏则书不可用
  bool okL = m_txt.renderPage(fbL, m_page);
  while (!okL) {
    Serial.printf("[epub] chapter %u exhausted/empty, advancing\n", (unsigned)m_chapter + 1);
    if (!nextChapter()) { m_ok = false; return false; }
    // 对页起点随章更新（进度存档/restartPair 要落在真正渲染的章上）
    m_pairStartCh = m_chapter;
    m_pairStart = m_txt.curFilePos();
    okL = m_txt.renderPage(fbL, m_page);
  }

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
  chk("render");
  return true;
}

bool EpubBook::restartPair() {
  if (!m_ok || m_pairStart < 0) return false;
  if (m_pairStartCh != m_chapter && !loadChapter(m_pairStartCh)) return false;
  m_chapterDone = false;
  m_page = m_pairStartPage;
  return m_txt.restorePair(m_pairStart, m_pairStartPage);
}

bool EpubBook::restorePair(uint16_t ch, long off, uint32_t page) {
  if (!m_ok) return false;
  if ((ch >= m_nChapters || !loadChapter(ch)) ) {
    // 章号非法或本章坏：退回第一章开头
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
