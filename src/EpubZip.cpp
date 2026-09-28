#include "EpubZip.h"
#include "Storage.h"
#include "SD_MMC.h"
#include "miniz/miniz.h"

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

// ===== 临时诊断（与 EpubBook.cpp 同款，定案后两处一起删）=====
static void chk(const char* tag) {
  bool ok = heap_caps_check_integrity_all(true);
  Serial.printf("[heap %s] %s | psram free=%u heap free=%u\n",
                ok ? "ok " : "BROKEN", tag,
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());
}
// ===== 诊断结束 =====

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
  // ===== 临时诊断：结构尺寸（比对被踩偏移 0x2558）=====
  Serial.printf("[dbg] sizeof(iter_state)=%u sizeof(tinfl)=%u off(inflator)=%u\n",
                (unsigned)sizeof(mz_zip_reader_extract_iter_state),
                (unsigned)sizeof(tinfl_decompressor),
                (unsigned)offsetof(mz_zip_reader_extract_iter_state, inflator));
  chk("open.start");
  // ===== 诊断结束 =====
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

// 解压条目 → SD 临时文件。
//
// 必须走迭代式 API（extract_iter_*），**不能**用 extract_to_callback/extract_to_mem：
// 那两条路把 tinfl_decompressor（≈8.2KB：m_look_up[3][1024] 霍夫曼表 + 树/码长表，
// 见 miniz.h struct tinfl_decompressor_tag）声明成栈上局部变量，而 Arduino loopTask
// 默认栈只有 8KB —— 进函数即栈溢出（P4 串口 b 切 EPUB 直接 reboot 的根因，坑 16）。
// 迭代式的解压器状态内嵌在 m_pAlloc 堆分配的 iter_state 里，栈上只留 1KB 拷贝缓冲。
// 顺带：拷贝/校验（CRC + 解压尺寸）由 iter_free 收尾，返回 MZ_FALSE = 本条目坏。
bool EpubZip::extractToFile(const char* zipPath, const char* outPath) {
  if (!m_ok) return false;
  m_fail = EXFAIL_NONE;
  int idx = mz_zip_reader_locate_file((mz_zip_archive*)m_zip, zipPath, NULL, 0);
  if (idx < 0) {
    Serial.printf("WARN: zip entry missing: %s\n", zipPath);
    m_fail = EXFAIL_ENTRY;
    return false;
  }
  File out = SD_MMC.open(outPath, FILE_WRITE);   // 覆盖写
  if (!out) {
    Serial.printf("WARN: cannot create temp %s\n", outPath);
    m_fail = EXFAIL_IO;
    return false;
  }
  uint32_t t0 = millis();
  // ===== 临时诊断 B：压缩字节指纹（与主机端 assets 拷贝比对，判定 SD 卡文件是否损坏）=====
  {
    mz_zip_archive_file_stat st;
    if (mz_zip_reader_file_stat((mz_zip_archive*)m_zip, (mz_uint)idx, &st)) {
      uint8_t hdr[30];
      if (sdReadFunc(&m_file, st.m_local_header_ofs, hdr, 30) == 30 &&
          hdr[0] == 'P' && hdr[1] == 'K') {
        uint16_t fnl = (uint16_t)(hdr[26] | (hdr[27] << 8));
        uint16_t exl = (uint16_t)(hdr[28] | (hdr[29] << 8));
        uint64_t dataOfs = st.m_local_header_ofs + 30 + fnl + exl;
        uint32_t crc = 0;
        uint8_t buf2[256];
        uint64_t left = st.m_comp_size;
        bool bad = false;
        while (left) {
          size_t chunk = left > sizeof(buf2) ? sizeof(buf2) : (size_t)left;
          if (sdReadFunc(&m_file, dataOfs, buf2, chunk) != chunk) { bad = true; break; }
          crc = (uint32_t)mz_crc32(crc, buf2, chunk);
          dataOfs += chunk;
          left -= chunk;
        }
        Serial.printf("[dbg] %s comp-crc32=%s0x%08x (comp=%lu)\n", zipPath,
                      bad ? "READFAIL-" : "", crc, (unsigned long)st.m_comp_size);
      }
    }
  }
  // ===== 诊断结束 =====
  mz_zip_reader_extract_iter_state* it =
      mz_zip_reader_extract_iter_new((mz_zip_archive*)m_zip, (mz_uint)idx, 0);
  if (!it) {                                      // 打不开条目（加密/分配失败/坏头）
    out.close();
    SD_MMC.remove(outPath);
    Serial.printf("WARN: extract %s -> %s failed (iter open)\n", zipPath, outPath);
    m_fail = EXFAIL_ENTRY;
    return false;
  }
  // ===== 临时诊断：打印 iter 状态块三指针 + 逐步校验（定案后删）=====
  Serial.printf("[dbg] it=%p read=%p write=%p | read_sz=%u write?=%d method=%u comp=%lu uncomp=%lu\n",
                it, it->pRead_buf, it->pWrite_buf,
                (unsigned)it->read_buf_size, it->pWrite_buf != NULL,
                (unsigned)it->file_stat.m_method,
                (unsigned long)it->file_stat.m_comp_size,
                (unsigned long)it->file_stat.m_uncomp_size);
  chk("iter_new");
  // ===== 诊断结束 =====
  bool failed = false;                            // 写 SD 失败（断卡/满卡）
  uint32_t own_crc = 0;                           // 坑 17：外部 CRC（栈变量，越界写够不着）
  uint8_t buf[1024];
  int iter = 0;
  for (;;) {
    size_t n = mz_zip_reader_extract_iter_read(it, buf, sizeof(buf));
    if (!n) break;                                // 解压完或出错，成败看 iter_free
    // ===== 临时诊断 C：第一轮读后立刻校验+十六进制转储（先于 out.write/printf，
    //      鉴别"tinfl 解压"还是"SD 写/串口"是凶手）=====
    if (iter == 0) {
      chk("read0");                               // 纯内存校验，先于一切 printf
      const uint8_t* p = (const uint8_t*)it;
      size_t off = 9496, len = 120;               // m_len_codes 尾部 + 结构体外毒丸区
      char line[104];
      for (size_t i = 0; i < len; i += 16) {
        int k = 0;
        for (size_t j = 0; j < 16 && i + j < len; j++)
          k += snprintf(line + k, sizeof(line) - k, "%02x ", p[off + i + j]);
        Serial.printf("[hex] +%4u: %s\n", (unsigned)(off + i), line);
      }
      chk("dump0");
      // 输出缓冲前 32 字节（验证解压内容本身是否正常）
      uint8_t o[33];
      memcpy(o, it->pWrite_buf, 32);
      o[32] = 0;
      Serial.printf("[hex] out[0..31]=%.32s\n", (const char*)o);
    }
    // ===== 诊断结束 =====
    if (out.write(buf, n) != n) { failed = true; break; }
    own_crc = (uint32_t)mz_crc32(own_crc, buf, n);
    // ===== 临时诊断：每轮迭代后校验 =====
    if (++iter <= 4) {
      chk("after_write");
      Serial.printf("[dbg] iter %d done\n", iter);
    }
    // ===== 诊断结束 =====
  }
  // ===== 临时诊断 E：iter_free 失败原因三角定位 =====
  //  status=0 已是 DONE，失败只剩尺寸/CRC 两个检查。对 pWrite_buf 实算 CRC，
  //  与 file_crc32、zip 存档 CRC 三方对比：谁错谁对一目了然。
  Serial.printf("[dbg] pre-free it=%p read=%p write=%p status=%d\n",
                it, it->pRead_buf, it->pWrite_buf, (int)it->status);
  {
    uint32_t got = (uint32_t)it->out_buf_ofs;
    uint32_t wcrc = (uint32_t)mz_crc32(0, (const uint8_t*)(const void*)it->pWrite_buf, (size_t)got);
    Serial.printf("[dbg] out_buf_ofs=%u expect=%u | file_crc32=0x%08x zip_crc=0x%08x pWrite_crc=0x%08x\n",
                  got, (unsigned)it->file_stat.m_uncomp_size,
                  (unsigned)it->file_crc32, (unsigned)it->file_stat.m_crc32, wcrc);
    // 输出缓冲前 64 字节转储（大章节不能全量打）
    const uint8_t* w = (const uint8_t*)(const void*)it->pWrite_buf;
    char line[104];
    for (uint32_t i = 0; i < got && i < 64; i += 16) {
      int k = 0;
      for (uint32_t j = 0; j < 16 && i + j < got; j++)
        k += snprintf(line + k, sizeof(line) - k, "%02x ", w[i + j]);
      Serial.printf("[out] +%3u: %s\n", (unsigned)i, line);
    }
    // 结构体尾 + slack 续段：9552=file_crc32，9616=pattern 周期第 2 个"56-59 槽"，
    // 若 9616-9619 也是 05 d5 7d b8 则 9552 属 pattern（字段被踩）；否则 9552=实算 CRC
    const uint8_t* p = (const uint8_t*)it;
    for (size_t i = 9536; i < 9664; i += 16) {
      int k = 0;
      for (size_t j = 0; j < 16; j++)
        k += snprintf(line + k, sizeof(line) - k, "%02x ", p[i + j]);
      Serial.printf("[hex] +%4u: %s\n", (unsigned)i, line);
    }
  }
  chk("pre-free");
  // ===== 诊断结束 =====
  // ===== 修复（坑 17/#371）：越界 pattern 踩了 file_crc32（偏移 9552 = tinfl 尾外首字段），
  //      miniz 从脏种子算 CRC 必不匹配。回填自己在调用侧累积的 CRC（栈变量，越界够不着），
  //      iter_free 的尺寸+CRC 双检全部保留。
  Serial.printf("[dbg] crc re-fill: miniz=0x%08x -> own=0x%08x (zip=0x%08x)\n",
                (unsigned)it->file_crc32, (unsigned)own_crc,
                (unsigned)it->file_stat.m_crc32);
  it->file_crc32 = own_crc;
  mz_bool ok = mz_zip_reader_extract_iter_free(it);
  chk("post-free");
  out.close();
  if (!ok || failed) {
    m_fail = failed ? EXFAIL_IO : EXFAIL_VERIFY;
    if (failed) {
      // SD 写已停摆（首次写超时 ≈5s，remove 还要再来一轮）→ 跳过删除：
      // 临时文件留着无害，下次 FILE_WRITE 打开即截断覆盖
      Serial.printf("WARN: extract %s -> %s failed (SD write dead, temp kept)\n",
                    zipPath, outPath);
    } else {
      SD_MMC.remove(outPath);
      Serial.printf("WARN: extract %s -> %s failed (crc/size verify)\n", zipPath, outPath);
    }
    return false;
  }
  Serial.printf("[epub] extracted %s: %ld B, %lu ms\n",
                zipPath, entrySize(zipPath), (unsigned long)(millis() - t0));
  return true;
}
