#include "Bookmarks.h"

#include <Preferences.h>

// 独立 Preferences 实例：eink.ino 的 prefs 句柄常开（begin 后不重开），
// 这里首次调用 begin 一次常驻（Preferences::begin 对已开启实例返回 false，
// 所以用 s_begun 挡重入）。NVS 操作带互斥，两个句柄同空间安全。
static Preferences s_bmp;
static bool s_begun = false;

static bool ensureBegin() {
  if (!s_begun) s_begun = s_bmp.begin("eink-book", false);
  return s_begun;
}

uint32_t Bookmarks::hashPath(const char* p) {
  uint32_t h = 2166136261u;
  while (p && *p) { h ^= (uint8_t)*p++; h *= 16777619u; }
  return h;
}

static void keyOf(const char* bookPath, char* out, size_t n) {
  snprintf(out, n, "bm%08lx", (unsigned long)Bookmarks::hashPath(bookPath));
}

static bool load(const char* bookPath, BmLoc* arr, int* n) {
  *n = 0;
  if (!ensureBegin()) return false;
  char k[16];
  keyOf(bookPath, k, sizeof(k));
  int len = s_bmp.getBytesLength(k);
  if (len <= 0) return true;                       // 无书签 = 空表（不是错误）
  if (len % (int)sizeof(BmLoc)) return false;      // 长度不对 = 损坏，当空处理由调用方兜底
  if (len > (int)sizeof(BmLoc) * Bookmarks::MAX) len = (int)sizeof(BmLoc) * Bookmarks::MAX;
  *n = len / (int)sizeof(BmLoc);
  return s_bmp.getBytes(k, arr, len) == (size_t)len;
}

static bool store(const char* bookPath, const BmLoc* arr, int n) {
  if (!ensureBegin()) return false;
  char k[16];
  keyOf(bookPath, k, sizeof(k));
  if (n <= 0) { s_bmp.remove(k); return true; }    // 删空 = 清 key
  return s_bmp.putBytes(k, arr, n * (int)sizeof(BmLoc)) == (unsigned)(n * (int)sizeof(BmLoc));
}

int Bookmarks::count(const char* bookPath) {
  BmLoc tmp[MAX];
  int n = 0;
  if (!load(bookPath, tmp, &n)) return 0;
  return n;
}

bool Bookmarks::get(const char* bookPath, int idx, BmLoc& out) {
  BmLoc tmp[MAX];
  int n = 0;
  if (!load(bookPath, tmp, &n) || idx < 0 || idx >= n) return false;
  out = tmp[idx];
  return true;
}

bool Bookmarks::add(const char* bookPath, const BmLoc& loc) {
  BmLoc tmp[MAX];
  int n = 0;
  if (!load(bookPath, tmp, &n)) n = 0;             // 损坏表当空，别挡添加
  if (n >= MAX) return false;
  tmp[n++] = loc;
  return store(bookPath, tmp, n);
}

void Bookmarks::clear(const char* bookPath) {
  store(bookPath, nullptr, 0);
}
