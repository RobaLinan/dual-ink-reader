#include "CjkFont.h"
#include "EinkPanel.h"   // 竖版页逻辑边界 PAGE_W/PAGE_H
#include "Storage.h"
#include "SD_MMC.h"
#include <ESP32epdx.h>   // Paint_SetPixel / BLACK
#include <string.h>
#include <stdio.h>
// 最后 include：Serial 重定向进日志环形缓冲（/debug 网页调试台，DESIGN §11）
#include "DbgLog.h"

CjkFont CJK;

static const size_t FONT_BYTES = (size_t)65536 * CjkFont::GLYPH_BYTES;  // 2097152

// ---------------- Unifont（classic 模式字库 + 家族缺字回退）----------------
bool CjkFont::begin(const char* path) {
  if (m_ok) return true;
  if (!Storage::begin()) return false;

  // 与 TxtBook 同理：挂载刚过首个 open 可能瞬时失败，退避重试再判缺文件
  for (int i = 0; i < 3 && !m_file; i++) {
    if (i) delay(100);
    m_file = SD_MMC.open(path, FILE_READ);
    if (!m_file) m_file = SD_MMC.open("/unifont.bin", FILE_READ);  // 旧布局兜底：根目录
  }
  if (!m_file) {
    Serial.printf("WARN: %s missing on SD - CJK will draw boxes\n", path);
  } else if (m_file.size() < FONT_BYTES) {
    Serial.printf("WARN: %s too small (%lu < %u) - bad file?\n", path,
                  (unsigned long)m_file.size(), (unsigned)FONT_BYTES);
    m_file.close();
  } else {
    Serial.printf("[mem] before unifont alloc: psram free=%u | heap free=%u\n",
                  (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());
    // 优先整块进 PSRAM：classic 查表变指针运算，页面渲染零额外 SD 流量
    m_ram = (uint8_t*)ps_malloc(FONT_BYTES);
    if (m_ram) {
      size_t off = 0;
      uint32_t t0 = millis();
      while (off < FONT_BYTES) {
        int n = m_file.read(m_ram + off, FONT_BYTES - off);
        if (n <= 0) break;
        off += (size_t)n;
      }
      if (off == FONT_BYTES) {
        m_file.close();
        Serial.printf("[font] unifont.bin -> PSRAM: %u bytes in %lu ms\n",
                      (unsigned)FONT_BYTES, (unsigned long)(millis() - t0));
      } else {
        Serial.printf("WARN: font read stopped at %u/%u bytes\n",
                      (unsigned)off, (unsigned)FONT_BYTES);
        free(m_ram);
        m_ram = nullptr;
        m_file.seek(0);
      }
    }
    if (!m_ram) {
      Serial.println("[font] PSRAM alloc failed - per-glyph SD streaming");
      // 回退：文件保持打开，逐字形 seek+read（单字形缓存）
    }
    m_ok = true;
  }

  // 扫 /fonts 发现字库包：font-<name>-<px>.bin（缺目录回根，兼容旧布局）
  m_nfam = 1;
  File root = SD_MMC.open("/fonts");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    root = SD_MMC.open("/");
  }
  if (root && root.isDirectory()) {
    File f;
    while ((f = root.openNextFile())) {
      const char* nm = f.name();
      const char* base = strrchr(nm, '/');
      base = base ? base + 1 : nm;
      size_t len = strlen(base);
      if (strncmp(base, "font-", 5) != 0 || len <= 9 ||
          strcmp(base + len - 4, ".bin") != 0) { f.close(); continue; }
      // 尾部 -<digits> 去掉得家族名
      const char* dash = base + len - 5;      // 指向 ".bin" 前一位
      while (dash > base && *dash >= '0' && *dash <= '9') dash--;
      if (dash <= base || *dash != '-') { f.close(); continue; }
      int nameLen = (int)(dash - base) - 5;   // 去掉 "font-" 与尾 '-'
      if (nameLen < 1) { f.close(); continue; }
      char nm16[16];
      memcpy(nm16, base + 5, nameLen < 15 ? nameLen : 15);
      nm16[nameLen < 15 ? nameLen : 15] = '\0';
      bool dup = strcmp(nm16, "classic") == 0;      // classic 是保留名
      for (int i = 1; !dup && i < m_nfam; i++)
        dup = strcmp(m_famNames[i], nm16) == 0;
      if (!dup && m_nfam < MAXFAM) {
        snprintf(m_famNames[m_nfam], sizeof(m_famNames[0]), "%s", nm16);
        m_nfam++;
      }
      f.close();
    }
    root.close();
  }
  Serial.printf("[font] families (%d):", m_nfam);
  for (int i = 0; i < m_nfam; i++) Serial.printf(" %s", m_famNames[i]);
  Serial.println();
  return m_ok;
}

const uint8_t* CjkFont::glyph(uint32_t cp) {
  if (!m_ok || cp >= 0x10000) return nullptr;
  if (m_ram) return m_ram + cp * GLYPH_BYTES;
  if (m_cached == cp) return m_buf;
  if (!m_file.seek(cp * GLYPH_BYTES)) return nullptr;
  if (m_file.read(m_buf, GLYPH_BYTES) != GLYPH_BYTES) return nullptr;
  m_cached = cp;
  return m_buf;
}

// ---------------- 家族（EPF1 包）----------------
const char* CjkFont::familyName(int i) const {
  if (i < 0 || i >= m_nfam) return "classic";
  return m_famNames[i];
}

int CjkFont::familyIndex(const char* name) const {
  if (!name) return -1;
  for (int i = 0; i < m_nfam; i++)
    if (strcmp(m_famNames[i], name) == 0) return i;
  return -1;
}

void CjkFont::unloadPack() {
  if (m_pack) { free(m_pack); m_pack = nullptr; }
  if (m_buk) { free(m_buk); m_buk = nullptr; m_bukLen = 0; m_bukId = -1; }
  if (m_packFile) m_packFile.close();
  m_fam = 0;
  m_baseY = m_desc = m_maxAdv = m_packSizePx = 0;
}

bool CjkFont::setFamily(int idx, uint8_t sizePx) {
  // 同家族同档且已装载 → 直接复用（EPUB 每章切换都调 setFamily，
  // 否则 kai 这类几 MB 的包会从 SD 反复整载，刷屏节奏被拖垮）
  if (idx > 0 && idx == m_fam && sizePx == m_packSizePx &&
      (m_pack || m_packFile))
    return true;
  unloadPack();
  if (idx <= 0) return true;                       // classic
  if (idx >= m_nfam) idx = 0;
  if (idx == 0) return true;

  char path[40];
  snprintf(path, sizeof(path), "/fonts/font-%s-%u.bin", m_famNames[idx], sizePx);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {                                   // 旧布局兜底：根目录
    snprintf(path, sizeof(path), "/font-%s-%u.bin", m_famNames[idx], sizePx);
    f = SD_MMC.open(path, FILE_READ);
  }
  if (!f) {
    Serial.printf("WARN: %s missing - fallback classic\n", path);
    return false;
  }
  uint8_t hdr[32];
  if (f.read(hdr, 32) != 32 || memcmp(hdr, "EPF1", 4) != 0 ||
      hdr[5] != sizePx) {
    Serial.printf("WARN: %s bad header - fallback classic\n", path);
    f.close();
    return false;
  }
  m_baseY   = hdr[6];
  m_desc    = hdr[12];
  m_maxAdv  = hdr[13];
  m_entriesOff = (uint32_t)hdr[20] | ((uint32_t)hdr[21] << 8) |
                 ((uint32_t)hdr[22] << 16) | ((uint32_t)hdr[23] << 24);
  if (m_entriesOff != 1060) {
    Serial.printf("WARN: %s unexpected entriesOff=%u - fallback classic\n",
                  path, (unsigned)m_entriesOff);
    f.close();
    return false;
  }
  if (f.read((uint8_t*)m_index, sizeof(m_index)) != (int)sizeof(m_index)) {
    Serial.printf("WARN: %s index read failed - fallback classic\n", path);
    f.close();
    return false;
  }
  size_t fsize = f.size();
  if (m_entriesOff >= fsize) { f.close(); return false; }

  m_packSizePx = sizePx;
  // 装载：整包 PSRAM → heap → SD 桶缓存
  m_pack = (uint8_t*)ps_malloc(fsize);
  if (!m_pack) m_pack = (uint8_t*)malloc(fsize);
  if (m_pack) {
    size_t off = 0;
    f.seek(0);
    while (off < fsize) {
      int n = f.read(m_pack + off, fsize - off);
      if (n <= 0) break;
      off += (size_t)n;
    }
    if (off == fsize) {
      m_packSize = fsize;
      f.close();
      m_fam = idx;
      Serial.printf("[font] pack %s loaded (%u bytes, base=%u desc=%u maxAdv=%u)\n",
                    path, (unsigned)fsize, m_baseY, m_desc, m_maxAdv);
      return true;
    }
    free(m_pack);
    m_pack = nullptr;
    Serial.printf("WARN: %s partial read (%u/%u) - SD bucket mode\n",
                  path, (unsigned)off, (unsigned)fsize);
  }

  // SD 模式：常驻文件 + 桶缓存（最大桶跨度一次性分配）
  size_t maxSpan = 0;
  for (int b = 0; b < 256; b++) {
    uint32_t lo = m_index[b], hi = m_index[b + 1];
    if (hi > lo && hi - lo > maxSpan) maxSpan = hi - lo;
  }
  m_buk = (uint8_t*)ps_malloc(maxSpan ? maxSpan : 1);
  if (!m_buk) m_buk = (uint8_t*)malloc(maxSpan ? maxSpan : 1);
  if (!m_buk) {
    Serial.println("WARN: bucket cache alloc failed - fallback classic");
    f.close();
    unloadPack();
    return false;
  }
  m_bukLen = maxSpan;
  m_bukId = -1;
  m_packFile = f;
  m_fam = idx;
  Serial.printf("[font] pack %s SD mode (bucket cache %u bytes)\n",
                path, (unsigned)maxSpan);
  return true;
}

bool CjkFont::loadBucket(int b) {
  if (m_bukId == b) return true;
  uint32_t lo = m_index[b], hi = m_index[b + 1];
  if (hi <= lo) { m_bukId = b; return true; }       // 空桶
  if (!m_packFile) return false;
  if (!m_packFile.seek(m_entriesOff + lo)) return false;
  if (m_packFile.read(m_buk, hi - lo) != (int)(hi - lo)) return false;
  m_bukId = b;
  return true;
}

// 查包记录。bits 指针在下次 loadBucket/unloadPack 前有效（调用方立即绘制）
bool CjkFont::findPack(uint32_t cp, Rec* r) {
  if (m_fam <= 0 || cp > 0xFFFF) return false;
  int b = cp >> 8;
  uint32_t lo = m_index[b], hi = m_index[b + 1];
  if (hi <= lo) return false;

  const uint8_t* base;
  if (m_pack) {
    base = m_pack + m_entriesOff + lo;
  } else {
    if (!loadBucket(b)) return false;
    base = m_buk;
    hi -= lo;                                      // 相对桶内长度
    lo = 0;
  }
  uint32_t pos = 0, end = (m_pack ? hi - lo : hi);
  while (pos + 7 <= end) {
    const uint8_t* p = base + pos;
    uint32_t c = p[0] | ((uint32_t)p[1] << 8);
    int w = p[5], h = p[6];
    if (c == cp) {
      r->adv  = p[2];
      r->xoff = (int8_t)p[3];
      r->yoff = (int8_t)p[4];
      r->w = w; r->h = h;
      r->bits = p + 7;
      return true;
    }
    if (c > cp) return false;                       // 有序，提前退出
    pos += 7 + (uint32_t)(((w + 7) / 8) * h);
  }
  return false;
}

int CjkFont::packAdv(uint32_t cp) {
  Rec r;
  if (!findPack(cp, &r)) return -1;
  return r.adv;
}

// ---------------- 绘制 ----------------
void CjkFont::drawGlyph(int x, int y, uint32_t cp, int size) {
  Rec r;
  if (findPack(cp, &r)) {                           // 家族包：紧凑 ink 对齐基线
    if (r.w == 0 || r.h == 0) return;               // 无墨（空格）
    int x0 = x + r.xoff, y0 = y + m_baseY + r.yoff;
    int rb = (r.w + 7) / 8;
    for (int row = 0; row < r.h; row++) {
      const uint8_t* line = r.bits + row * rb;
      for (int col = 0; col < r.w; col++) {
        if (!(line[col >> 3] & (0x80 >> (col & 7)))) continue;
        int px = x0 + col, py = y0 + row;
        // ink 可能比 adv 宽（悬挂极值）：自兜底，Paint_SetPixel 的边界判断是
        // > 不是 ≥，逻辑 x=480/y=648 会写穿显存行缓冲（坐标是竖版页的，
        // 旋转映射后 x→显存行、y→显存列，SetPixel 自己的边界检查拦不住）
        if (px > EinkPanel::PAGE_W - 1 || py > EinkPanel::PAGE_H - 1) continue;
        Paint_SetPixel(px, py, BLACK);
      }
    }
    return;
  }
  if (m_fam > 0) {
    // 家族模式缺字 → Unifont 回退：把字形挪到与包 CJK 视觉对齐的位置
    // （包 CJK ink 顶 ≈ 基线-7/8*size；Unifont 满格从行顶起画，直接画会高一截）
    y += m_baseY - (size * 7 / 8);
  }

  if (size <= 0 || size > 64) size = 16;
  const uint8_t* g = glyph(cp);
  if (!g) {                       // BMP 外或字库不可用：空心方框当缺字标记
    int t = (size + 15) / 16;     // 边框粗细随档位：16→1px，24/32→2px
    for (int c = 0; c < size; c++) {
      for (int k = 0; k < t; k++) {
        Paint_SetPixel(x + c, y + k, BLACK);
        Paint_SetPixel(x + c, y + size - 1 - k, BLACK);
        Paint_SetPixel(x + k, y + c, BLACK);
        Paint_SetPixel(x + size - 1 - k, y + c, BLACK);
      }
    }
    return;
  }
  for (int dy = 0; dy < size; dy++) {
    int sy = dy * 16 / size;   // 最近邻：16→24 = 1.5×，16→32 = 2×
    uint16_t row = ((uint16_t)g[2 * sy] << 8) | g[2 * sy + 1];
    for (int dx = 0; dx < size; dx++) {
      int sx = dx * 16 / size;
      if (row & (0x8000 >> sx)) Paint_SetPixel(x + dx, y + dy, BLACK);
    }
  }
}
