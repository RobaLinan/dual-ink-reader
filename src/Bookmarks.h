#ifndef BOOKMARKS_H
#define BOOKMARKS_H

#include <Arduino.h>

// 书签存储（2026-10-12）：每本书一条 NVS blob（命名空间 "eink-book"，
// 与 eink.ino 进度存档同空间不同 key，两份 Preferences 句柄共存无冲突）。
//   key = "bm" + FNV-1a(书路径) 的 8 位十六进制（路径可能含中文/长名，NVS key
//         最长 15 字符，放不下全路径 → 哈希作键；哈希撞车只丢书签不炸进度）
//   blob = BmLoc 定长记录 ×N（顺序即添加顺序，N ≤ MAX）
// 选 NVS 而不选 SD 文件：坑 18 定案 SD 写入物理锁卡，书签这种高频小写绝不能碰 SD。
//
// 坐标语义与 eink.ino 进度存档/对页历史栈完全一致：
//   off = 对页起点的文件偏移（EPUB = 章内偏移），page = 对页页号，
//   ch  = 章号（EPUB ≥0；TXT = -1），permille = 添加时的全书千分比（显示用）。

struct BmLoc {
  long off;
  uint32_t page;
  int16_t ch;
  uint16_t permille;   // 0..1000；0xFFFF = 未知
};

class Bookmarks {
public:
  static const int MAX = 16;      // 每书书签上限（NVS blob ≤ 16×12B = 192B）

  static uint32_t hashPath(const char* path);   // FNV-1a（TxtBook 目录缓存键也用）
  static int count(const char* bookPath);
  static bool get(const char* bookPath, int idx, BmLoc& out);
  static bool add(const char* bookPath, const BmLoc& loc);  // false = 满或写失败
  static void clear(const char* bookPath);                  // 删书时清书签
};

#endif  // BOOKMARKS_H
