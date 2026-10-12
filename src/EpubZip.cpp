#include "EpubZip.h"
#include "Storage.h"
#include "SD_MMC.h"
#include "miniz/miniz.h"
// 最后 include：Serial 重定向进日志环形缓冲（/debug 网页调试台，DESIGN §11）
#include "DbgLog.h"

// ===== miniz 32 位堆越界缓解（永久修复，上游 richgel999/miniz#371）=====
// xtensa 32 位目标上 tinfl_decompress 会写越 tinfl_decompressor 结构体尾部（64 位
// 主机不复现；ESP-IDF 堆毒丸报 Bad head/tail，紧跟结构体之后，值为确定性字节序列，
// 与上游观测 0x0A040210 同模式）。tinfl_bit_buf_t 在 32 位为 uint32 是其根因线索。
// 上游无已发布修复，官方缓解 = 结构体与输出缓冲各超配 4096 字节。
// 这里在 m_pAlloc 钩子对所有 miniz 分配统一 +4096；mz_zip_reader_init_internal
// 只在 m_pAlloc==NULL 时装默认值，故 open() 里预设。零补丁 vendored miniz。
#include <esp_heap_caps.h>
#include <stddef.h>
static const size_t MZ_ALLOC_SLACK = 4096;
static void* mz_alloc_padded(void* op, size_t items, size_t size) {
  (void)op;
  return heap_caps_malloc(items * size + MZ_ALLOC_SLACK, MALLOC_CAP_8BIT);
}
static void mz_free_padded(void* op, void* ptr) { (void)op; free(ptr); }
static void* mz_realloc_padded(void* op, void* ptr, size_t items, size_t size) {
  (void)op;
  return heap_caps_realloc(ptr, items * size + MZ_ALLOC_SLACK, MALLOC_CAP_8BIT);
}

// ===== 临时诊断已清理（P4 定案 2026-09-28：堆校验/hex 转储/CRC 三角定位全删，
//      坑 17 的 own_crc 回填是正式修复，保留在下方）=====

// SD File 读回调：miniz 需要"任意偏移随机读"，SDMMC 的 seek+read 正好满足
static size_t sdReadFunc(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
  File* f = (File*)opaque;
  if (!f->seek((uint32_t)ofs)) return 0;
  return f->read((uint8_t*)buf, n);
}

bool EpubZip::open(const char* path) {
  if (m_ok) return true;
  if (!Storage::begin()) return false;
  for (int i = 0; i < 3 && !m_file; i++) {   // 同 TxtBook：挂载刚过首开偶发失败
    if (i) delay(100);
    m_file = SD_MMC.open(path, FILE_READ);
  }
  if (!m_file) {
    Serial.printf("WARN: %s not found on SD card\n", path);
    return false;
  }
  m_zip = calloc(1, sizeof(mz_zip_archive));
  if (!m_zip) { m_file.close(); return false; }
  mz_zip_archive* zip = (mz_zip_archive*)m_zip;
  zip->m_pRead = sdReadFunc;
  zip->m_pIO_opaque = &m_file;
  // ===== miniz 32 位越界缓解：所有 miniz 分配超配 4096（见文件头注释）=====
  zip->m_pAlloc = mz_alloc_padded;
  zip->m_pFree = mz_free_padded;
  zip->m_pRealloc = mz_realloc_padded;
  // MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY：省一次排序分配，locate 仍可用
  if (!mz_zip_reader_init(zip, m_file.size(), MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
    Serial.printf("WARN: %s not a zip (err=%d)\n", path, (int)zip->m_last_error);
    free(m_zip); m_zip = nullptr; m_file.close();
    return false;
  }
  m_ok = true;
  return true;
}

void EpubZip::close() {
  if (m_zip) {
    mz_zip_reader_end((mz_zip_archive*)m_zip);
    free(m_zip); m_zip = nullptr;
  }
  if (m_file) m_file.close();
  m_ok = false;
}

long EpubZip::entrySize(const char* zipPath) {
  if (!m_ok) return -1;
  mz_zip_archive_file_stat st;
  int idx = mz_zip_reader_locate_file((mz_zip_archive*)m_zip, zipPath, NULL, 0);
  if (idx < 0) return -1;
  if (!mz_zip_reader_file_stat((mz_zip_archive*)m_zip, idx, &st)) return -1;
  return (long)st.m_uncomp_size;
}

bool EpubZip::forEach(void (*fn)(const char* path, uint32_t size, void* ctx),
                      void* ctx) {
  if (!m_ok || !fn) return false;
  mz_zip_archive* zip = (mz_zip_archive*)m_zip;
  mz_uint n = mz_zip_reader_get_num_files(zip);
  for (mz_uint i = 0; i < n; i++) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, i, &st)) continue;
    if (st.m_is_directory) continue;
    fn(st.m_filename, (uint32_t)st.m_uncomp_size, ctx);
  }
  return true;
}

// 解压条目 → 调用方指定 FS（EPUB 用片内 LittleFS）上的临时文件。
//
// 必须走迭代式 API（extract_iter_*），**不能**用 extract_to_callback/extract_to_mem：
// 那两条路把 tinfl_decompressor（≈8.2KB：m_look_up[3][1024] 霍夫曼表 + 树/码长表，
// 见 miniz.h struct tinfl_decompressor_tag）声明成栈上局部变量，而 Arduino loopTask
// 默认栈只有 8KB —— 进函数即栈溢出（P4 串口 b 切 EPUB 直接 reboot 的根因，坑 16）。
// 迭代式的解压器状态内嵌在 m_pAlloc 堆分配的 iter_state 里，栈上只留 1KB 拷贝缓冲。
// 顺带：拷贝/校验（CRC + 解压尺寸）由 iter_free 收尾，返回 MZ_FALSE = 本条目坏。
bool EpubZip::extractToFile(const char* zipPath, const char* outPath, fs::FS& outFs) {
  if (!m_ok) return false;
  m_fail = EXFAIL_NONE;
  int idx = mz_zip_reader_locate_file((mz_zip_archive*)m_zip, zipPath, NULL, 0);
  if (idx < 0) {
    Serial.printf("WARN: zip entry missing: %s\n", zipPath);
    m_fail = EXFAIL_ENTRY;
    return false;
  }
  File out = outFs.open(outPath, FILE_WRITE);     // 覆盖写（EPUB 走 LittleFS，见坑 18）
  if (!out) {
    Serial.printf("WARN: cannot create temp %s\n", outPath);
    m_fail = EXFAIL_IO;
    return false;
  }
  uint32_t t0 = millis();
  mz_zip_reader_extract_iter_state* it =
      mz_zip_reader_extract_iter_new((mz_zip_archive*)m_zip, (mz_uint)idx, 0);
  if (!it) {                                      // 打不开条目（加密/分配失败/坏头）
    out.close();
    outFs.remove(outPath);
    Serial.printf("WARN: extract %s -> %s failed (iter open)\n", zipPath, outPath);
    m_fail = EXFAIL_ENTRY;
    return false;
  }
  bool failed = false;                            // 临时文件写失败（断卡/满卡，现走 LittleFS 罕见）
  uint32_t own_crc = 0;                           // 坑 17：外部 CRC（栈变量，越界写够不着）
  uint8_t buf[1024];
  for (;;) {
    size_t n = mz_zip_reader_extract_iter_read(it, buf, sizeof(buf));
    if (!n) break;                                // 解压完或出错，成败看 iter_free
    if (out.write(buf, n) != n) { failed = true; break; }
    own_crc = (uint32_t)mz_crc32(own_crc, buf, n);
  }
  // ===== 修复（坑 17/#371）：越界 pattern 踩了 file_crc32（偏移 9552 = tinfl 尾外首字段），
  //      miniz 从脏种子算 CRC 必不匹配。回填自己在调用侧累积的 CRC（栈变量，越界够不着），
  //      iter_free 的尺寸+CRC 双检全部保留。
  it->file_crc32 = own_crc;
  mz_bool ok = mz_zip_reader_extract_iter_free(it);
  out.close();
  if (!ok || failed) {
    m_fail = failed ? EXFAIL_IO : EXFAIL_VERIFY;
    if (failed) {
      // 写已停摆（历史教训：SD 写死时首写超时 ≈5s，remove 还要再来一轮）→
      // 跳过删除：临时文件留着无害，下次 FILE_WRITE 打开即截断覆盖
      Serial.printf("WARN: extract %s -> %s failed (write dead, temp kept)\n",
                    zipPath, outPath);
    } else {
      outFs.remove(outPath);
      Serial.printf("WARN: extract %s -> %s failed (crc/size verify)\n", zipPath, outPath);
    }
    return false;
  }
  Serial.printf("[epub] extracted %s: %ld B, %lu ms\n",
                zipPath, entrySize(zipPath), (unsigned long)(millis() - t0));
  return true;
}
