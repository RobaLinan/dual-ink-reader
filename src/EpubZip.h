#ifndef EPUB_ZIP_H
#define EPUB_ZIP_H

#include <Arduino.h>
#include <FS.h>

// P4：EPUB = zip 容器。miniz 读取器封装（vendored src/miniz，见 src/VENDOR.md）：
//   open()        = 打开 SD 上的 .epub，读中央目录（miniz 惰性读，不整载）
//   extractToFile = 流式解压一个条目到调用方指定的 FS 临时文件（EPUB 走片内
//                   LittleFS，不写 SD——DESIGN 坑 18：SD 写会物理锁卡；坑 6：
//                   >280KB 章节先落临时文件再处理，绝不在 MCU 上整章常驻 RAM；
//                   逐块 inflate，峰值 RAM 只有 miniz 窗口 + 写缓冲）
// miniz 通过 mz_zip_reader_init + 自定义 m_pRead 回调直接读 SD File，
// 不经过 stdio（SD_MMC 的文件句柄与 FILE* 不通用）。

class EpubZip {
public:
  // 上次 extractToFile 的失败类型。EXFAIL_IO = 临时文件写失败（原方案写 SD 会在
  // 驱动层 0x107/0x109 停摆 → 调用方走 Storage::recover 重挂自救；坑 18 后临时
  // 文件在片内 LittleFS，此值几乎不再出现，保留语义）；条目缺失/校验败与写无关。
  enum ExtractFail { EXFAIL_NONE = 0, EXFAIL_ENTRY, EXFAIL_IO, EXFAIL_VERIFY };

  bool open(const char* path);          // false = 打不开/不是 zip
  void close();
  bool isOpen() const { return m_ok; }
  ExtractFail lastFail() const { return m_fail; }

  // 按路径解压条目（zip 内路径，大小写敏感）到 outFs。false = 无此条目/解压失败。
  // 目标文件已存在则截断覆盖。
  bool extractToFile(const char* zipPath, const char* outPath, fs::FS& outFs);

  // 条目原始大小（解压后），-1 = 无此条目
  long entrySize(const char* zipPath);

  // 一次 O(N) 遍历中央目录：fn(条目路径, 解压后大小, ctx)，非目录条目。
  // 用于建"章路径 → 大小"权重映射（进度%按 spine 章权重估算）；
  // 按索引 stat 避免 locate_file 线性扫 × N 章的 O(N²) SD 往返。
  bool forEach(void (*fn)(const char* path, uint32_t size, void* ctx), void* ctx);

private:
  File m_file;
  bool m_ok = false;
  void* m_zip = nullptr;   // mz_zip_archive（miniz 头里 typedef，这里隐藏类型）
  ExtractFail m_fail = EXFAIL_NONE;
};

#endif  // EPUB_ZIP_H
