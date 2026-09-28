#include "TxtBook.h"
#include "Storage.h"
#include "CjkFont.h"
#include "EinkPanel.h"
#include "SD_MMC.h"
#include <ESP32epdx.h>
#include <string.h>
#include <stdio.h>

// === 排版常量：按像素宽计量（DESIGN §5.5 中英混排）===
// 画布是竖版页 EinkPanel::PAGE_W(480) × PAGE_H(648)（短边为横边），由
// PAGE_ROTATE 旋转进 648×480 显存 —— 这里的行宽/页底全按竖版页逻辑坐标算。
static const int TXT_X       = 8;
static const int TXT_Y0      = 32;   // 页眉(8+16) + 分隔线(26) 之下
static const int HARD_PX     = EinkPanel::PAGE_W - 16;  // 硬上限（档位无关）：8+464=472，
                                    //   末像素 471 < 480 不越界
                                    //   （Paint_SetPixel 边界判断是 > 不是 ≥，
                                    //    逻辑 x=480 会写穿显存行缓冲）
static const int MAXTOK      = 64;   // 单行 token 上限（实算 ≤45，防御）

// 三档字号的运行期参数（TxtBook::setTier 设置）：
//   S(16): Font16 原生 + Unifont16 | 行高18 33行 | 软 464-16=448
//   M(24): Font24 原生 + Unifont1.5× | 行高28 21行 | 软 464-24=440
//   L(32): Font16 2× + Unifont 2×  | 行高36 16行 | 软 464-32=432
//   （行数由 updateGeometry 按 TXT_BOT/TXT_Y0/档宽算，换页高只动 TXT_BOT）
//   软预算 = HARD_PX - s_cjkPx：给避头悬挂留出最大字宽，悬挂后仍 ≤464。
//   家族包模式（CJK.packActive()）下行几何另算：行距按包的基线/行盒度量，
//   软预算按包 maxAdv；步进逐字取真实 advance（proportional）。
static int s_asciiPx = 11, s_cjkPx = 16;
static int s_linePx  = 448;          // 软预算
static int s_lineH   = 18;           // 行高
static int s_maxline = 33;           // 每页行数
static int s_asciiMode = 1;          // 0=Font24 原生 / 1=Font16 原生 / 2=Font16 2× 放大
static const int TXT_BOT = EinkPanel::PAGE_H - 18;   // 正文墨迹底界（页底留 18px 余量）

// 行缓冲 = UTF-8 字节流；token 表记每段的起点/前缀像素宽/文件位置（页尾回退用）
static char s_line[256];
static int s_lineLen = 0, s_px = 0, s_lines = 0;
static bool s_pendingSpace = false;
static long s_lineStart = 0, s_pageStart = 0;

struct Tok { uint16_t off; uint16_t prePx; uint32_t wStart; };
static Tok s_tok[MAXTOK];
static int s_ntok = 0;

// ---- UTF-8 解码 ----
// 从字节流解一个码点：*used = 消耗字节数；非法序列 → '?' 且消耗 1
static uint32_t utf8At(const char* s, int len, int* used) {
  const uint8_t* p = (const uint8_t*)s;
  if (len <= 0) { *used = 0; return '?'; }
  uint8_t b = p[0];
  if (b < 0x80) { *used = 1; return b; }
  int extra; uint32_t cp;
  if ((b & 0xE0) == 0xC0)      { cp = b & 0x1F; extra = 1; }
  else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; }
  else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; extra = 3; }
  else                         { *used = 1; return '?'; }
  if (extra >= len) { *used = 1; return '?'; }
  for (int i = 1; i <= extra; i++) {
    if ((p[i] & 0xC0) != 0x80) { *used = i; return '?'; }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *used = extra + 1;
  return cp;
}

// 读一个"源码点"：UTF-8 解码 + 常见西文标点归一到 ASCII（英文排版走 Font16 不变）。
// raw[]/rawLen 带出原始字节（宽字符 token 要原样入行缓冲）。返回 -1 = EOF
static int txtGetCp(File& f, char raw[4], int* rawLen) {
  uint8_t b;
  if (f.read(&b, 1) != 1) return -1;
  raw[0] = (char)b;
  *rawLen = 1;
  if (b < 0x80) {
    if (b == 0x7F) return '?';
    return b;
  }
  int extra; uint32_t cp;
  if ((b & 0xE0) == 0xC0)      { cp = b & 0x1F; extra = 1; }
  else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; }
  else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; extra = 3; }
  else                         { return '?'; }
  for (int i = 1; i <= extra; i++) {
    uint8_t c2;
    if (f.read(&c2, 1) != 1) return -1;
    raw[i] = (char)c2;
    *rawLen = i + 1;
    if ((c2 & 0xC0) != 0x80) return '?';
    cp = (cp << 6) | (c2 & 0x3F);
  }
  switch (cp) {
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x00A0: return ' ';
    default: return (int)cp;
  }
}

// ---- 分类 ----
static bool isWide(uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x303E) ||   // 部首/符号/注音/假名起
         (cp >= 0x3041 && cp <= 0x33FF) ||   // 假名、兼容
         (cp >= 0x3400 && cp <= 0x4DBF) ||   // 扩展 A
         (cp >= 0x4E00 && cp <= 0x9FFF) ||   // 统一汉字
         (cp >= 0xAC00 && cp <= 0xD7A3) ||   // 谚文
         (cp >= 0xF900 && cp <= 0xFAFF) ||   // 兼容汉字
         (cp >= 0xFE30 && cp <= 0xFE6F) ||
         (cp >= 0xFF00 && cp <= 0xFF60) ||   // 全角形式
         (cp >= 0xFFE0 && cp <= 0xFFE6);
}

// 避头尾（DESIGN §5.5）：行首禁则 = 收尾类标点，行尾禁则 = 起始类括号
static bool isBadStart(uint32_t cp) {
  switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF01: case 0xFF1F:
    case 0xFF1B: case 0xFF1A: case 0xFF09: case 0xFF0D: case 0x3011:
    case 0x3009: case 0x300B: case 0x300D: case 0x300F: case 0x2026:
    case ',': case '.': case ';': case ':': case '!': case '?':
    case ')': case ']': case '}': case '%': case '\'': case '"':
      return true;
    default: return false;
  }
}
static bool isBadEnd(uint32_t cp) {
  switch (cp) {
    case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010:
    case 0x3014: case 0xFF08: case '(': case '[': case '{':
      return true;
    default: return false;
  }
}

// 当前字体状态下 cp 的排版步进：家族包 = 真实 advance（proportional，
// 含弯引号/重音等全部码点）；包缺字回退档位估值；classic = 档位固定值。
static int advPx(uint32_t cp) {
  if (CJK.packActive()) {
    int a = CJK.packAdv(cp);
    if (a >= 0) return a;
    return isWide(cp) ? s_cjkPx : s_asciiPx;
  }
  return cp < 0x80 ? s_asciiPx : s_cjkPx;
}
static int pxOf(const char* b, int len) {
  int px = 0, i = 0;
  while (i < len) { int u; px += advPx(utf8At(b + i, len - i, &u)); i += u; }
  return px;
}

// Font16 点阵按 2× 最近邻放大（L 档英文：字高 32、步进 22，与 Font16 数据布局一致：
//   11 列/行 → 每行 ceil(11/8)=2 字节，MSB=最左）
static void drawAscii16x2(int x, int y, char c) {
  if (c < ' ' || c > '~') c = '?';
  const int CW = 11, CH = 16, RB = 2;
  const uint8_t* p = Font16.table + (c - ' ') * RB * CH;
  for (int r = 0; r < CH; r++)
    for (int col = 0; col < CW; col++)
      if (p[r * RB + col / 8] & (0x80 >> (col % 8)))
        for (int dy = 0; dy < 2; dy++)
          for (int dx = 0; dx < 2; dx++)
            Paint_SetPixel(x + col * 2 + dx, y + r * 2 + dy, BLACK);
}

// ---- 行上落 token ----
// 画出当前行并开新行。false = 页满不画不重置（调用方回退文件，内容不丢）
static bool flushLine() {
  if (s_lines >= s_maxline) return false;
  s_line[s_lineLen] = '\0';
  int y = TXT_Y0 + s_lines * s_lineH;
  int x = TXT_X, i = 0;
  while (i < s_lineLen) {               // 混排绘制
    int u;
    uint32_t cp = utf8At(s_line + i, s_lineLen - i, &u);
    if (CJK.packActive()) {             // 家族包：全部走包字形（对齐基线），真实步进
      CJK.drawGlyph(x, y, cp, s_cjkPx); // 缺字回退 Unifont 放大 / 方框
      x += advPx(cp);
    } else if (cp < 0x80) {
      if (s_asciiMode == 2)      drawAscii16x2(x, y, (char)cp);
      else if (s_asciiMode == 0) Paint_DrawChar(x, y, (char)cp, &Font24, BLACK, WHITE);
      else                       Paint_DrawChar(x, y, (char)cp, &Font16, BLACK, WHITE);
      x += s_asciiPx;
    } else {
      CJK.drawGlyph(x, y, cp, s_cjkPx);
      x += s_cjkPx;
    }
    i += u;
  }
  s_lines++;
  s_lineLen = 0; s_px = 0; s_ntok = 0;
  return true;
}

// 追加一个 token（含前导空格）。false = 页满且刷不下去（行里已有内容 → 回退安全）
static bool appendTok(const char* b, int len, int px, uint32_t wStart) {
  if (s_ntok >= MAXTOK && !flushLine()) return false;
  if (s_lineLen + len + 2 > (int)sizeof(s_line) && !flushLine()) return false;
  Tok t;
  t.off = s_lineLen;
  t.prePx = s_px;
  t.wStart = wStart;
  if (s_ntok == 0) s_lineStart = wStart;
  bool sep = s_pendingSpace && s_px > 0;
  if (sep) { s_line[s_lineLen++] = ' '; s_px += advPx(' '); t.off = s_lineLen - 1; }
  memcpy(s_line + s_lineLen, b, len);
  s_lineLen += len;
  s_px += px;
  s_tok[s_ntok++] = t;
  s_pendingSpace = false;
  return true;
}

// 前缀字节宽不超过 budgetPx 的切割点（超长词硬切用），至少切 1 个码点
static int cutPoint(const char* b, int len, int budgetPx) {
  int i = 0, px = 0;
  while (i < len) {
    int u;
    uint32_t cp = utf8At(b + i, len - i, &u);
    if (px + advPx(cp) > budgetPx && i > 0) break;
    px += advPx(cp);
    i += u;
  }
  return i;
}

static char s_carry[200];   // 推挤暂存：承载 token ≤ 单词缓冲 192B（见 placeToken）

// 把一个 token 放进当前行。放不下：避头悬挂 → 推挤（上一 token 挪下去同行）→
// 换行 → 超长词硬切。返回 false = 本页已满未放入（调用方停止读、页尾回退）
static bool placeToken(const char* b, int len, int px, uint32_t wStart) {
  for (int guard = 0; guard < 12; guard++) {
    bool sep = s_pendingSpace && s_px > 0;
    int need = (sep ? advPx(' ') : 0) + px;
    if (s_px + need <= s_linePx) return appendTok(b, len, px, wStart);

    if (s_ntok == 0) {                       // 空行放不下 = 超长词硬切
      int cut = cutPoint(b, len, s_linePx);
      int cpx = pxOf(b, cut);
      if (!appendTok(b, cut, cpx, wStart)) return false;
      if (s_lines >= s_maxline) return false;      // 回退到本行首（前缀已在缓冲）
      flushLine();
      b += cut; len -= cut; px -= cpx; wStart += cut;
      s_pendingSpace = false;
      continue;
    }

    int u;
    uint32_t firstCp = utf8At(b, len, &u);
    Tok last = s_tok[s_ntok - 1];
    uint32_t lastCp = '?';
    { int i = last.off, lu;                       // 行内最后一个 token 的末码点
      while (i < s_lineLen) { lastCp = utf8At(s_line + i, s_lineLen - i, &lu); i += lu; } }

    // 避头：行首禁则字符挂到行尾（吞右边距，最多 16px，不越屏）
    if (isBadStart(firstCp) && s_px + px <= HARD_PX)
      return appendTok(b, len, px, wStart);

    // 需要换行。避头（前面还有东西可挪）或避尾（上一是开括号）→ 推挤同行
    if (s_ntok >= 2 && (isBadStart(firstCp) || isBadEnd(lastCp)) &&
        s_lines < s_maxline) {                 // 页满就别动行（回退即安全）
      int carryOff = last.off;
      int carryLen = s_lineLen - carryOff;
      int carryPx = s_px - last.prePx;
      bool keepPending = s_pendingSpace;         // 属于原 token 的前导空格，别丢
      if (carryLen > 0 && carryLen <= (int)sizeof(s_carry)) {
        if (s_line[carryOff] == ' ') {           // 行首不留空格：剥掉 carry 的前导空格
          carryOff++; carryLen--; carryPx -= advPx(' ');
        }
        memcpy(s_carry, s_line + carryOff, carryLen);
        s_ntok--;                                // 摘下 carry
        s_lineLen = last.off;
        s_px = last.prePx;
        flushLine();                             // 页满已预检，必成功
        if (!appendTok(s_carry, carryLen, carryPx, last.wStart)) return false;
        s_pendingSpace = keepPending;
        continue;                                // 再试放原 token（新行几乎必放得下）
      }
    }
    // 普通换行
    if (!flushLine()) return false;
    s_pendingSpace = false;
  }
  return false;   // 不变量兜底（到不了）
}

// 词（ASCII/窄字符连续段）入行。false = 页满未放入（调用方须停止读取）
static bool placeWord(const char* w, int len, uint32_t wStart) {
  return len == 0 || placeToken(w, len, pxOf(w, len), wStart);
}

// ---- TxtBook ----
// 行几何随字号档 + 家族联动：classic 用档位表；家族包用包内基线/行盒/maxAdv 度量。
// 每页行数统一按墨迹盒算（换页高只动 TXT_BOT）：
//   n = (TXT_BOT - TXT_Y0 - 盒高) / 行高 + 1   —— 横版 462 时正好还原 24/15/12
static void updateGeometry() {
  int box;   // 行内墨迹盒高：classic = 档位字高；家族包 = 基线 + 最大下伸
  if (CJK.packActive()) {
    box = CJK.baselineY() + CJK.maxDescent();
    s_lineH = box + s_cjkPx / 8;                    // 行距 16→+2 / 24→+3 / 32→+4
  } else {
    box = s_cjkPx;
    switch (s_cjkPx) {
      case 24: s_lineH = 28; break;
      case 32: s_lineH = 36; break;
      default: s_lineH = 18; break;
    }
  }
  int n = (TXT_BOT - TXT_Y0 - box) / s_lineH + 1;
  s_maxline = n > 0 ? n : 1;
  // 软预算与 classic 同规则：悬挂路径另有 <=HARD_PX 显式检查兜底，
  // 不必按包 maxAdv 收缩（U+2E3B 三倍宽破折号那种 78px 特例会白扔行宽）
  s_linePx = HARD_PX - s_cjkPx;
}

void TxtBook::setTier(uint8_t sizePx) {
  switch (sizePx) {
    case 24: s_asciiPx = 17; s_cjkPx = 24; s_asciiMode = 0; break;
    case 32: s_asciiPx = 22; s_cjkPx = 32; s_asciiMode = 2; break;
    default: s_asciiPx = 11; s_cjkPx = 16; s_asciiMode = 1; break;
  }
  if (CJK.packActive())              // 家族包按新档位重载（缺该档文件 → 自动回 classic）
    CJK.setFamily(CJK.currentFamily(), s_cjkPx);
  updateGeometry();
}

bool TxtBook::setFamily(const char* name) {
  int idx = name ? CJK.familyIndex(name) : 0;
  if (idx < 0) idx = 0;
  bool ok = CJK.setFamily(idx, s_cjkPx);
  updateGeometry();
  return ok;
}

// 关句柄：切书与 SD 重挂载前调用。只动 m_file/m_ok —— m_page/m_pairStart
// 保留，切走前 saveProgress() 还要读它们（进度不丢）
void TxtBook::close() {
  if (m_file) m_file.close();
  m_ok = false;
}

bool TxtBook::open(const char* path) { return open(SD_MMC, path); }

bool TxtBook::open(fs::FS& fs, const char* path) {
  if (m_ok) close();          // 允许重开（EpubBook 每章换临时文本文件）
  // 流式状态每次 open 归零（原逻辑只在"开着时重开"分支里重置 → 先 close 再
  // open 会把上一章的 m_hitEof/m_pairStart 带进新章，EPUB 右页会误判章末）；
  // m_page/m_pairStart 的恢复由调用方显式 restorePair 做，open 不该继承
  m_hitEof = false;
  m_page = 0;
  m_pairStart = -1;
  if (!Storage::begin()) return false;

  setTier(TXT_SIZE_PX);            // 默认档（TxtBook.h 可改），按键阶段可运行期切换

  // 挂载刚过就开文件偶发失败（卡初始化完还在忙，2026-09-24 实测：挂载重试
  // 成功后首开 /books/test_cn.txt 失败、下一个候选却秒开）。100ms 退避重试
  for (int i = 0; i < 3 && !m_file; i++) {
    if (i) delay(100);
    m_file = fs.open(path, FILE_READ);
  }
  if (!m_file) {
    Serial.printf("WARN: cannot open %s\n", path);
    return false;
  }
  m_ok = true;
  if (!m_quiet)   // EPUB 章节临时文件（setQuiet 在 open 前置位）不刷这行
    Serial.printf("[SD] %s: %lu bytes\n", path, (unsigned long)m_file.size());

  const char* base = path[0] == '/' ? path + 1 : path;
  if (!m_titleOverridden)
    snprintf(m_title, sizeof(m_title), "%s", base);

  if (!m_quiet) {
    char dump[401];
    int n = m_file.read((uint8_t*)dump, 400);
    if (n > 0) {
      dump[n] = '\0';
      Serial.println("--- head 400B ---");
      Serial.println(dump);
      Serial.println("--- head end ---");
    }
    m_file.seek(0);
  }

  CJK.begin("/fonts/unifont.bin");   // 缺了只警告，CJK 画方框，英文不受影响
  return true;
}

bool TxtBook::renderPair(uint8_t* fbL, uint8_t* fbR) {
  if (!m_ok) return false;
  m_pairStart = m_file.position();          // 记录本对页起点（字体切换时回跳重排）
  m_pairStartPage = m_page;
  bool ok = renderPage(fbL, m_page) && renderPage(fbR, m_page + 1);
  if (!ok) { m_ok = false; return false; }
  m_page += 2;
  return true;
}

// 回到最近一次 renderPair 的起点（切换字体/字号后调用，再 renderPair 即同页重排）
bool TxtBook::restartPair() {
  if (!m_ok || m_pairStart < 0) return false;
  if (!m_file.seek(m_pairStart)) return false;
  m_page = m_pairStartPage;
  return true;
}

// 深睡唤醒恢复：seek 回存档的对页起点并登记为"当前对页"（不渲染）
bool TxtBook::restorePair(long off, uint32_t page) {
  if (!m_ok || off < 0) return false;
  if (!m_file.seek(off)) return false;
  m_page = page;
  m_pairStart = off;
  m_pairStartPage = page;
  return true;
}

// 覆盖页眉标题（EPUB 用书名+章节号，不用临时文件名）
void TxtBook::setTitle(const char* t) {
  if (t) {
    snprintf(m_title, sizeof(m_title), "%s", t);
    m_titleOverridden = true;
  } else {
    m_title[0] = '\0';
    m_titleOverridden = false;
  }
}

// 页眉混排（文件名可能带中文），固定 Font16 / 16px 不随档位。
// 竖版页只有 480px 宽（长文件名+页码放不下）：放不下就截断 —— 逻辑 x=480 会
// 写穿显存（Paint_SetPixel 边界是 > 不是 ≥，旋转后 x 映射成显存行号）
static void drawHeader(const char* hdr) {
  int i = 0, x = TXT_X;
  while (hdr[i]) {
    int u;
    uint32_t cp = utf8At(hdr + i, (int)strlen(hdr) - i, &u);
    int adv = cp < 0x80 ? 11 : 16;
    if (x + adv > EinkPanel::PAGE_W) break;   // 末像素 ≤479 才画
    if (cp < 0x80) Paint_DrawChar(x, 8, (char)cp, &Font16, BLACK, WHITE);
    else CJK.drawGlyph(x, 8, cp, 16);
    x += adv;
    i += u;
  }
}

// 空白页（页眉+分隔线，正文全白）：EPUB 章节在右页中间结束
void TxtBook::renderBlank(uint8_t* fb, uint32_t pageNo) {
  Paint_SelectImage(fb);
  Paint_Clear(WHITE);
  char hdr[48];
  snprintf(hdr, sizeof(hdr), "%s   page %lu", m_title, (unsigned long)pageNo);
  drawHeader(hdr);
  Paint_DrawLine(TXT_X, 26, EinkPanel::PAGE_W - 1 - TXT_X, 26,
                 BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
}

// 渲染一页（页眉+分隔线+正文）。文件位置持续推进 → 左右屏天然连续；
// 页尾半行回退不丢字（s_lineStart = 当前行首 token 的文件位置）。
// m_loop=false（EPUB）：EOF 后不再回卷，m_hitEof 置位返回
bool TxtBook::renderPage(uint8_t* fb, uint32_t pageNo) {
  if (!m_file) return false;
  Paint_SelectImage(fb);
  Paint_Clear(WHITE);

  char hdr[48];
  snprintf(hdr, sizeof(hdr), "%s   page %lu", m_title, (unsigned long)pageNo);
  drawHeader(hdr);                              // 页眉也走混排（文件名可能带中文）
  Paint_DrawLine(TXT_X, 26, EinkPanel::PAGE_W - 1 - TXT_X, 26,
                 BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);

  m_hitEof = false;
  s_lineLen = 0; s_px = 0; s_lines = 0; s_ntok = 0;
  s_pendingSpace = false;
  s_pageStart = m_file.position();
  s_lineStart = s_pageStart;

  char word[192];
  int wlen = 0;
  uint32_t wStart = s_pageStart;
  bool eof = false, rewound = false;
  uint32_t t0 = millis();
  long before;

  while (s_lines < s_maxline) {
    char raw[4]; int rawLen = 0;
    before = m_file.position();
    int cp = eof ? -1 : txtGetCp(m_file, raw, &rawLen);
    if (cp < 0) {
      eof = true;
      m_hitEof = true;                       // EPUB 章末信号（loop 模式下同置位，无害）
      if (s_lines == 0 && s_lineLen == 0 && wlen == 0) {
        if (!m_loop || rewound) break;       // 非循环：没有更多内容；空文件兜底
        rewound = true;
        m_file.seek(0);                      // EOF 回卷：TXT 翻过末页后从头再来
        eof = false;
        m_hitEof = false;
        continue;
      }
    } else if (cp == '\n') {
      // 任一步骤因页满失败 → 立即停读；缓冲里有未画内容，页尾回退不丢字
      if (!placeWord(word, wlen, wStart)) break;
      wlen = 0;
      if (!flushLine()) break;
      s_lineStart = m_file.position();
      s_pendingSpace = false;
    } else if (cp == ' ' || cp == '\t' || cp == '\r' || cp < 0x20 || cp == 0x3000) {
      if (!placeWord(word, wlen, wStart)) break;
      wlen = 0;
      s_pendingSpace = true;
    } else if (isWide(cp)) {
      // 宽字符（汉字/全角标点）：独立 token，两端都是断点
      if (!placeWord(word, wlen, wStart)) break;
      wlen = 0;
      if (!placeToken(raw, rawLen, s_cjkPx, (uint32_t)before)) break;
    } else {
      // ASCII/窄字符并入当前词（词内不断）
      if (wlen == 0) wStart = before;
      if (wlen + rawLen < (int)sizeof(word)) {
        memcpy(word + wlen, raw, rawLen);
        wlen += rawLen;
      } else {
        if (!placeWord(word, wlen, wStart)) break;   // 超长 token 防溢出
        wlen = 0;
        if (!placeToken(raw, rawLen, advPx(cp), (uint32_t)before)) break;
      }
    }
    if (eof) {
      if (wlen) { placeWord(word, wlen, wStart); wlen = 0; }
      if (s_lineLen > 0 && s_lines < s_maxline) flushLine();
      break;
    }
  }
  // 页满但末行没画完 → 回退到行首，下页从这里重读（不丢不重）
  if (s_lineLen > 0) m_file.seek(s_lineStart);

  Serial.printf("[txt] page %lu: pos %ld->%ld, %d lines, %lu ms\n",
                (unsigned long)pageNo, s_pageStart, (long)m_file.position(),
                s_lines, (unsigned long)(millis() - t0));
  return s_lines > 0;
}
