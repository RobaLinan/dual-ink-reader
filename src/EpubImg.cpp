#include "EpubImg.h"
#include <LittleFS.h>
#include <FS.h>
#include <ESP32epdx.h>       // Paint_SetPixel / BLACK / WHITE（GUI_Paint 全局画布）
#include "tjpgd/tjpgd.h"     // ChaN TJpgDec R0.03（vendored，见 src/VENDOR.md）
#include <PNGdec.h>          // ~/Arduino/libraries/PNGdec（Larry Bank，BSD）

// === 4×4 Bayer 有序抖动 ===
// 阈值矩阵 0..15：黑 if 亮度 < (bayer+1)*16。有序抖动给插画灰阶的观感远好于硬阈值
// （Rackham 这类线描插画的淡墨过渡不丢），代价是每个目标像素一次表查。
static const uint8_t BAYER4[4][4] = {
  {  0,  8,  2, 10 },
  { 12,  4, 14,  6 },
  {  3, 11,  1,  9 },
  { 15,  7, 13,  5 },
};

// 缩放映射上下文：源（解码输出）坐标 → 目标屏上矩形
struct ImgCtx {
  int x, y;    // 目标左上（已含居中偏移）
  int tw, th;  // 目标尺寸
  int sw, sh;  // 源尺寸
};

// 显存边界兜底（Paint_SetPixel 内部也有判断，双保险防越界写）
static const int EPD_W = 480, EPD_H = 648;

// 一个源像素落屏（可能是放大 span：目标矩形 [dx0,dx1)×[dy0,dy1)）。
// 抖动阈值按目标左上像素取 —— 同一 span 内一致，多源像素挤一个目标像素时
// 重复写同值也无害（SetPixel 是位或进显存）。RGB565 直接取亮度（JPEG 与
// PNG 两路解码输出统一成 565，省一次分支）。
static inline int lum565(uint16_t v) {
  int r = (v & 0xF800) >> 8, g = (v & 0x07E0) >> 3, b = (v & 0x001F) << 3;
  return (r * 77 + g * 150 + b * 29) >> 8;
}
static void putSpan565(const ImgCtx& c, int sx, int sy, uint16_t rgb565) {
  int dx0 = c.x + (int)((int64_t)sx * c.tw / c.sw);
  int dy0 = c.y + (int)((int64_t)sy * c.th / c.sh);
  int dx1 = c.x + (int)((int64_t)(sx + 1) * c.tw / c.sw);
  int dy1 = c.y + (int)((int64_t)(sy + 1) * c.th / c.sh);
  if (dx1 <= dx0) dx1 = dx0 + 1;
  if (dy1 <= dy0) dy1 = dy0 + 1;
  if (lum565(rgb565) < (BAYER4[dy0 & 3][dx0 & 3] + 1) * 16) {
    for (int dy = dy0; dy < dy1 && dy < EPD_H; dy++)
      for (int dx = dx0; dx < dx1 && dx < EPD_W; dx++)
        Paint_SetPixel(dx, dy, BLACK);
  }
}

// 由原始尺寸算目标矩形：宽优先贴满 maxW，超高再压 maxH；小图不放大（原生尺寸）
static void fitRect(ImgCtx& c, int iw, int ih, int x, int y, int maxW, int maxH) {
  int tw = iw, th = ih;
  if (tw > maxW) { tw = maxW; th = (int)((int64_t)ih * maxW / iw); }
  if (th > maxH) { th = maxH; tw = (int)((int64_t)iw * maxH / ih); }
  if (tw < 1) tw = 1;
  if (th < 1) th = 1;
  c.x = x + (maxW - tw) / 2;      // 行内居中
  c.y = y;
  c.tw = tw; c.th = th;
}

// === 文件头探测 ===
static bool probeJpeg(File& f, int* w, int* h) {
  // 调用方已确认 FF D8（SOI），从 SOI 之后开始扫
  // 扫 marker 段找 SOFn（C0-C7 除 C4/C8/CC）。限制 64KB 内 —— SOF 必在文件头附近
  for (int guard = 0; guard < 256 && f.available() >= 4; guard++) {
    int c = f.read();
    if (c != 0xFF) continue;
    int m = f.read();
    if (m == 0xFF) continue;              // 填充 FF
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD9)) continue;  // 无长度段
    uint8_t len[2];
    if (f.read(len, 2) != 2) return false;
    int segLen = (len[0] << 8) | len[1];
    if (segLen < 2) return false;
    if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      uint8_t dims[5];
      if (f.read(dims, 5) != 5) return false;
      *h = (dims[1] << 8) | dims[2];
      *w = (dims[3] << 8) | dims[4];
      return *w > 0 && *h > 0;
    }
    if (!f.seek(f.position() + segLen - 2)) return false;
  }
  return false;
}

static bool probePng(File& f, int* w, int* h) {
  uint8_t b[24];
  if (f.read(b, 24) != 24) return false;
  if (b[0] != 0x89 || b[1] != 'P' || b[2] != 'N' || b[3] != 'G') return false;
  if (memcmp(b + 4, "\r\n\x1a\n", 4) != 0) return false;
  if (memcmp(b + 12, "IHDR", 4) != 0) return false;
  *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
  *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
  return *w > 0 && *h > 0;
}

bool EpubImg::probe(const char* path, int* w, int* h) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  uint8_t b[2];
  bool ok = false;
  if (f.read(b, 2) == 2 && b[0] == 0xFF && b[1] == 0xD8)
    ok = probeJpeg(f, w, h);            // 文件指针已在 SOI 之后
  else if (f.seek(0))
    ok = probePng(f, w, h);             // PNG 需从头读 24B 头
  f.close();
  return ok;
}

// === JPEG：ChaN TJpgDec（vendored）===
// 块回调式：输入回调喂压缩流，输出回调收 16×16 RGB565 MCU 块（tjpgdcnf.h 里
// JD_FORMAT=1）—— 全程无需整图缓冲，峰值 RAM = 工作区 3.5KB（JD_FASTDECODE=1，
// TJPGD_WORKSPACE_SIZE）+ 输入缓冲。scale = 1/2^N 降采样，大图源像素太多时
// 先降档再软缩放，省解码功夫。
// （坑 19：首版接 SDK 的 esp_rom_tjpgd（S3 ROM 真身）在 prepare 即 JDR_INP 返
//  回——根因是 ChaN 约定的"NULL buf = 跳过 n 字节"（APP0/EXIF 段跳过）没实现，
//  ROM 与源码同此约定；ROM 无从调试，弃用 vendor ChaN R0.03，行为可控可查。）
struct JpgIo {
  File f;
  ImgCtx c;
};
static size_t jpgIn(JDEC* jd, uint8_t* buf, size_t n) {
  JpgIo* io = (JpgIo*)jd->device;
  if (!buf) {                    // ChaN 约定：buf=NULL = 从流里丢弃 n 字节
    uint8_t sink[64];            // （JFIF APP0 / EXIF APP1 段跳过走这条路；
    size_t left = n;             //  直接 read(nullptr) 会返回错误 → JDR_INP 首杀）
    while (left) {
      size_t chunk = left > sizeof(sink) ? sizeof(sink) : left;
      size_t r = io->f.read(sink, chunk);
      if (!r) return 0;          // EOF → JDR_INP
      left -= r;
    }
    return n;
  }
  return io->f.read(buf, n);     // 短读/0 = EOF → JDR_INP 中断
}
static int jpgOut(JDEC* jd, void* bitmap, JRECT* r) {
  ImgCtx* c = &((JpgIo*)jd->device)->c;
  const uint16_t* px = (const uint16_t*)bitmap;
  // JRECT 是闭区间（tjpgd: rect.right = x + rx - 1），且 workbuf 里 rx×ry 个像素
  // 紧凑排列、步进 = rx = right-left+1。少写 +1 → 步进比实际窄 1，每行起点左移
  // 一格、越积越斜，16×16 MCU 块内呈对角涂抹 + 丢末行末列（首版屏上"插图糊成
  // 一片看不清"的根因，坑 21）
  int rw = r->right - r->left + 1, rh = r->bottom - r->top + 1;
  for (int y = 0; y < rh; y++) {
    for (int x = 0; x < rw; x++) {
      putSpan565(*c, r->left + x, r->top + y, px[(size_t)y * rw + x]);
    }
  }
  return 1;
}

static bool drawJpeg(File& f, int iw, int ih, int x, int y, int maxW, int maxH) {
  uint8_t* work = (uint8_t*)heap_caps_malloc(TJPGD_WORKSPACE_SIZE,
                                             MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
  if (!work) return false;
  JpgIo io;
  io.f = f;
  fitRect(io.c, iw, ih, x, y, maxW, maxH);
  JDEC jd;
  // Bodmer 补丁版 jd_prepare 会**保留**进参的 jd.swap（先读后 memset 再回填），
  // 即字节序开关由调用方负责。不清零 = 拿栈上垃圾当开关，非 0 时输出整体字节
  // 交换的 RGB565 → lum565 量出的亮度错乱（图发灰/发花）。本项目恒要原生序
  memset(&jd, 0, sizeof(jd));
  JRESULT r = jd_prepare(&jd, jpgIn, work, TJPGD_WORKSPACE_SIZE, &io);
  if (r == JDR_OK) {
    // 源大于 2× 目标就降采样解码（有序抖动下看不出差别，省一半以上像素）
    uint8_t n = 0;
    while (n < 3 && (int)(jd.width >> n) > io.c.tw * 2 &&
                    (int)(jd.height >> n) > io.c.th * 2) n++;
    io.c.sw = (int)(jd.width >> n);
    io.c.sh = (int)(jd.height >> n);
    r = jd_decomp(&jd, jpgOut, n);
  }
  free(work);
  if (r != JDR_OK) {
    Serial.printf("WARN: jpeg decode err=%d (%ux%u)\n", (int)r,
                  (unsigned)jd.width, (unsigned)jd.height);
    return false;
  }
  return true;
}

// === PNG：PNGdec ===
// PNGdec 是"每行回调"式：pDraw 给一行（或几行拼块）原生像素，getLineAsRGB565
// 转 RGB565 后按目标采样。PNG 对象 ≈35KB（32KB inflate 窗口 + 调色板 + 行缓冲），
// 栈上放不下（loopTask 栈 8KB，坑 16 同因），必须堆分配，用完即释放。
struct PngIo {
  PNG* png;
  ImgCtx c;
};
static void* pngOpen(const char* name, int32_t* pSize) {
  File* f = new File(LittleFS.open(name, FILE_READ));
  if (!f || !*f) { delete f; return nullptr; }
  *pSize = (int32_t)f->size();
  return f;
}
static int32_t pngRead(PNGFILE* pFile, uint8_t* buf, int32_t len) {
  return ((File*)pFile->fHandle)->read(buf, len);
}
static int32_t pngSeek(PNGFILE* pFile, int32_t pos) {
  return ((File*)pFile->fHandle)->seek(pos) ? pos : 0;
}
static void pngClose(void* handle) {
  File* f = (File*)handle;
  if (f) { f->close(); delete f; }
}
static int pngDrawCb(PNGDRAW* p) {
  PngIo* io = (PngIo*)p->pUser;
  int w = p->iWidth;
  if (w > 4096) return 1;                 // 防御：异常宽的图放弃转换
  // PNGRGB565 按整对/整组像素步进：4bpp `x+=2`、2bpp `x+=4`，**奇数宽度会多写
  // 1~3 个像素**（库缺陷，未修）。alice_rackham 的 title.png = 373 宽（奇数）
  // → 恰好 2 字节写穿 malloc(373*2) 的尾部魔数 0xbaad5678 → 下次 free 报
  // CORRUPT HEAP + multi_heap_free 断言重启（坑 21）。多留 4 像素吸收越界写；
  // 下面只取前 w 个，多出来的垃圾像素不进画布。
  uint16_t* line = (uint16_t*)malloc(((size_t)w + 4) * sizeof(uint16_t));
  if (!line) return 1;
  io->png->getLineAsRGB565(p, line, PNG_RGB565_LITTLE_ENDIAN, 0xFFFF);  // 白底
  for (int sx = 0; sx < w; sx++)
    putSpan565(io->c, sx, p->y, line[sx]);
  free(line);
  return 1;
}

static bool drawPng(const char* path, int iw, int ih, int x, int y, int maxW, int maxH) {
  PNG* png = (PNG*)ps_malloc(sizeof(PNG));   // ~35KB，PSRAM 充裕（unifont 载完还剩数 MB）
  if (!png) png = (PNG*)malloc(sizeof(PNG));
  if (!png) return false;
  memset(png, 0, sizeof(PNG));               // 防解码器读到 malloc 垃圾当状态
  PngIo io;
  io.png = png;
  fitRect(io.c, iw, ih, x, y, maxW, maxH);
  io.c.sw = iw; io.c.sh = ih;                // PNG 不硬件降采样，源 = 全尺寸
  bool ok = false;
  if (png->open(path, pngOpen, pngClose, pngRead, pngSeek, pngDrawCb) == PNG_SUCCESS) {
    if (png->isInterlaced()) {
      Serial.println("WARN: interlaced PNG unsupported - skipped");
    } else {
      ok = png->decode(&io, 0) == PNG_SUCCESS;
      if (!ok) Serial.printf("WARN: png decode err=%d\n", png->getLastError());
    }
    png->close();
  }
  free(png);
  return ok;
}

bool EpubImg::draw(const char* path, int x, int y, int maxW, int maxH) {
  int iw = 0, ih = 0;
  if (!probe(path, &iw, &ih)) return false;
  if (iw > 8192 || ih > 8192) return false;  // 防御：离谱尺寸不喂解码器
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  uint8_t b[2];
  bool isJpeg = f.read(b, 2) == 2 && b[0] == 0xFF && b[1] == 0xD8;
  f.seek(0);
  bool ok = isJpeg ? drawJpeg(f, iw, ih, x, y, maxW, maxH)
                   : drawPng(path, iw, ih, x, y, maxW, maxH);
  f.close();
  return ok;
}
