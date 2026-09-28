#ifndef EPUB_ZIP_H
#define EPUB_ZIP_H

#include <Arduino.h>
#include <FS.h>

// P4：EPUB = zip 容器。miniz 读取器封装（vendored src/miniz，见 src/VENDOR.md）：
//   open()        = 打开 SD 上的 .epub，读中央目录（miniz 惰性读，不整载）
//   extractToFile = 流式解压一个条目到 SD 临时文件（DESIGN 坑 6：>280KB 章节
//                   先落 SD 再处理，绝不在 MCU 上整章常驻 RAM；逐块 inflate，
//                   峰值 RAM 只有 miniz 窗口 + 写缓冲）
// miniz 通过 mz_zip_reader_init + 自定义 m_pRead 回调直接读 SD File，
// 不经过 stdio（SD_MMC 的文件句柄与 FILE* 不通用）。

class EpubZip {
public:
  // 上次 extractToFile 的失败类型。EXFAIL_IO = SD 写失败（写在驱动层 0x107/0x109
  // 停摆）→ 调用方可走 Storage::recover 重挂自救；条目缺失/校验败与 SD 无关。
  enum ExtractFail { EXFAIL_NONE = 0, EXFAIL_ENTRY, EXFAIL_IO, EXFAIL_VERIFY };

  bool open(const char* path);          // false = 打不开/不是 zip
  void close();
  bool isOpen() const { return m_ok; }
  ExtractFail lastFail() const { return m_fail; }

  // 按路径解压条目（zip 内路径，大小写敏感）。false = 无此条目/解压失败。
  // 目标文件已存在则截断覆盖。
  bool extractToFile(const char* zipPath, const char* outPath);

  // 条目原始大小（解压后），-1 = 无此条目
  long entrySize(const char* zipPath);

private:
  File m_file;
  bool m_ok = false;
  void* m_zip = nullptr;   // mz_zip_archive（miniz 头里 typedef，这里隐藏类型）
  ExtractFail m_fail = EXFAIL_NONE;
};

#endif  // EPUB_ZIP_H
