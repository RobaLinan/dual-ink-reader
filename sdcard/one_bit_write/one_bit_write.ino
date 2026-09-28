// SD 读写专项测试 v3 —— 硬件排查工具，不随主固件烧录。
// 引脚与正式接线一致（WIRING §1：38/39/40），是 DESIGN 坑 18 的复测工具。
//
// v3 流程（每档写完整档读回校验）：
//   读专项 → A 512B×4 → B/C/D 4KB 块（16KB/256KB/2MB）
//   有挂才追加：E 512B 小块 2MB（判块大小）｜ F 节流写 4KB+200ms（判供电/编程时间）
//   全过才追加：G 32KB 大块 2MB（测写速上限）
//   20MHz 有挂 → 自动降 10MHz 全流程重跑
// 另有：截获 IDF 驱动层返回码、出错 end+重挂（CMD0）自动恢复、每 64KB 打进度
//
// 判读（配合 WIRING §4 制板 checklist）：
//   20MHz 全过            → 写路径硬件 OK，可回填 DESIGN §5.7 待补项
//   仅 10MHz 过           → 信号/供电边际 → 卡座 VDD 去耦、CMD/D0 47k 上拉
//   两档 0x109 后死锁     → 坑 18 复现 → 换卡排除 / 断电重插再试
//   挂载即 0x107          → CMD 浮空或卡未就绪（上拉、接触、卡是否插好）
//
// 串口：发 `r` = 不复位重跑一轮（换卡 / 重新插卡后用）
// ⚠ 每次"卡已锁死"后必须断电重插再测，不重插的下一轮结果无意义
#include "FS.h"
#include "SD_MMC.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdarg.h>

#define PIN_CLK 38
#define PIN_CMD 39
#define PIN_D0  40

// 最大分块 32KB；必须来自片内 malloc（SDMMC 走 DMA，PSRAM 缓冲会被拒）
static const size_t kBufSize = 32 * 1024;
static uint8_t *g_buf = nullptr;

// Arduino 预处理器会把函数原型统一插到第一个函数之前（那时 struct StageRes 还没
// 定义），不前置声明会编译不过 —— 这行必须放在第一个函数定义之前
struct StageRes;

// 缓冲必须落在片内 DMA 可达内存（SDMMC 走 DMA）；S3 上超过 ~4KB 的 malloc
// 默认可能被放到 PSRAM，那样写出去的数据不可靠，所以显式指定能力位
static bool allocBuf() {
  if (g_buf) return true;
  g_buf = (uint8_t *)heap_caps_malloc(kBufSize, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!g_buf) g_buf = (uint8_t *)malloc(kBufSize);
  if (!g_buf) return false;
  fillPattern(g_buf, kBufSize, 0);
  return true;
}

// ============ ① 驱动层错误码截获 ============
static volatile int g_drvErr = 0;
static char g_drvMsg[160] = "";
// 粘性 CRC 标志：一次写里可能先 0x109 再 0x107，g_drvErr 只留最后一个，
// 0x109 一旦出现就记住（坑18 的指纹），供结论区判读
static volatile bool g_drvSaw0x109 = false;

static void errClear() { g_drvErr = 0; g_drvMsg[0] = '\0'; g_drvSaw0x109 = false; }

// 从 IDF 日志里抠返回码：`returned 0x109`（hex）/ `failed (265)`（dec）
static void noteDrvLine(const char *line) {
  const char *p = strstr(line, "returned 0x");
  int v;
  if (p) {
    v = (int)strtoul(p + strlen("returned 0x"), nullptr, 16);
  } else if ((p = strstr(line, "failed (0x"))) {
    v = (int)strtoul(p + strlen("failed (0x"), nullptr, 16);
  } else if ((p = strstr(line, "failed ("))) {
    v = atoi(p + strlen("failed ("));
  } else {
    return;  // unhandled 掩码之类的行不带错误码，不记
  }
  g_drvErr = v;
  if (v == 0x109) g_drvSaw0x109 = true;
  snprintf(g_drvMsg, sizeof(g_drvMsg), "%s", line);
  // 去掉行尾换行，方便单行打印
  for (char *c = g_drvMsg; *c; c++) if (*c == '\n' || *c == '\r') { *c = '\0'; break; }
}

static int hookVprintf(const char *fmt, va_list args) {
  char line[256];
  va_list copy;
  va_copy(copy, args);
  vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  if (strstr(line, "sdmmc") || strstr(line, "diskio")) noteDrvLine(line);
  return vprintf(fmt, args);
}

static const char *errName(int e) {
  switch (e) {
    case 0x101: return "内存不足";
    case 0x102: return "参数错";
    case 0x103: return "状态错";
    case 0x104: return "尺寸错";
    case 0x106: return "不支持";
    case 0x107: return "超时（卡无应答/信号断）";
    case 0x108: return "应答格式错";
    case 0x109: return "CRC 错（应答或数据）";
    default:    return "未知";
  }
}

// 单行打出"驱动层到底报了什么"，替代人肉去上面翻 E(...) 行
static void printDrvErr(const char *prefix) {
  if (g_drvErr || g_drvMsg[0]) {
    Serial.printf("%s 驱动层 0x%x (%s)\n", prefix, g_drvErr, errName(g_drvErr));
    if (g_drvMsg[0]) Serial.printf("   └ %s\n", g_drvMsg);
  } else {
    Serial.printf("%s 驱动层无报错（文件层返回 0 字节）\n", prefix);
  }
}

// ============ 挂载（坑 15 同款：指数退避 + 每次重拉上拉） ============
static bool mountOnce(uint32_t freqKHz) {
  // CMD/D0 内部上拉兜底：模块没焊总线上拉时 CMD 浮空 → 初始化 0x107。
  // end() 会拆引脚配置，所以每次重试都要重拉（DESIGN 坑 15 教训）
  pinMode(PIN_CMD, INPUT_PULLUP);
  pinMode(PIN_D0, INPUT_PULLUP);
  if (!SD_MMC.setPins(PIN_CLK, PIN_CMD, PIN_D0)) return false;
  if (!SD_MMC.begin("/sdcard", /*mode1bit=*/true, /*format=*/false, freqKHz))
    return false;
  File probe = SD_MMC.open("/");          // 防"半挂载"：begin 过了但首开失败
  if (!probe) {
    Serial.println("    （半挂载：begin 说成功、首开根目录却失败 → 已 end() 拆掉，重试）");
    SD_MMC.end();
    return false;
  }
  probe.close();
  return true;
}

static bool mountAt(uint32_t freqKHz, int attempts) {
  static const uint16_t kBackoffMs[] = {200, 400, 800, 1600};
  for (int i = 0; i < attempts; i++) {
    if (i) { SD_MMC.end(); delay(kBackoffMs[(i - 1) > 3 ? 3 : (i - 1)]); }
    errClear();
    if (mountOnce(freqKHz)) return true;
    Serial.printf("  ✖ 挂载失败 @%u kHz（第 %d/%d）", freqKHz, i + 1, attempts);
    printDrvErr("");
  }
  return false;
}

// 出错后的恢复：end + 重挂 = CMD0 重初始化卡（DESIGN 的 Storage::recover 同款）
static bool recover(uint32_t freqKHz) {
  Serial.println("  → 尝试恢复：end + 重挂（CMD0 重初始化卡）");
  SD_MMC.end();
  delay(300);
  if (mountAt(freqKHz, 3)) { Serial.println("  ✔ 恢复成功，可继续下一档"); return true; }
  Serial.println("  ✖ 恢复失败 = 卡已锁死，只能断电重插");
  return false;
}

// ============ 数据图案：按 512B 扇区编号取值，校验时可独立重算 ============
static void fillPattern(uint8_t *b, size_t len, size_t off0) {
  size_t i = 0;
  while (i < len) {
    size_t blk = 512 - ((off0 + i) & 511);
    if (blk > len - i) blk = len - i;
    memset(b + i, (uint8_t)((off0 + i) >> 9), blk);
    i += blk;
  }
}
static bool checkPattern(const uint8_t *b, size_t len, size_t off0) {
  for (size_t i = 0; i < len; i++)
    if (b[i] != (uint8_t)((off0 + i) >> 9)) return false;
  return true;
}

// ============ ② 阶段一：读专项（DESIGN §5.7 待补项之一） ============
static bool stageRead() {
  // 目录枚举本身也是读路径：必须在枚举前清标志，否则枚举期的 0x107 被漏掉，
  // 上一轮日志里"读 OK / 根目录无文件"其实底下全是 sdmmc_read_blocks failed
  errClear();
  File dir = SD_MMC.open("/");
  if (!dir || !dir.isDirectory()) {
    Serial.println("  ✖ 读：根目录打不开");
    printDrvErr("   ");
    return false;
  }
  String bigPath;
  uint64_t bigSize = 0;
  int n = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      n++;
      if (f.size() > bigSize) { bigSize = f.size(); bigPath = f.path(); }
    }
    f.close();
  }
  dir.close();
  if (g_drvErr) {          // 枚举阶段读挂了 → 不能当成"没有文件"
    Serial.println("  ✖ 读：枚举根目录时驱动层出错（读路径已挂）");
    printDrvErr("   ");
    return false;
  }
  if (!bigSize) { Serial.println("  ○ 读：根目录无文件（先跑一次写阶段再来读）"); return true; }

  File r = SD_MMC.open(bigPath.c_str(), FILE_READ);
  if (!r) { Serial.println("  ✖ 读：打开最大文件失败"); printDrvErr("   "); return false; }
  uint32_t t0 = millis(), got = 0;
  uint32_t crc = 0;
  errClear();
  while (r.available()) {
    int n2 = r.read(g_buf, kBufSize);
    if (n2 <= 0) break;
    for (int i = 0; i < n2; i++) crc = crc * 31u + g_buf[i];
    got += n2;
  }
  r.close();
  uint32_t ms = millis() - t0;
  if (got != bigSize || g_drvErr) {
    Serial.printf("  ✖ 读：只读到 %u/%u 字节\n", got, (unsigned)bigSize);
    printDrvErr("   ");
    return false;
  }
  Serial.printf("  ✔ 读：%s %u KB，%.2f MB/s（crc=%08x）\n", bigPath.c_str(),
                got / 1024, ms ? (got / 1048576.0) / (ms / 1000.0) : 0.0, crc);
  return true;
}

// ============ ② 阶段二：写 + 整档读回校验 ============
struct StageRes {
  bool fail = false;
  bool openFail = false;
  unsigned long done = 0;      // 失败前成功写入的字节
  unsigned long verifyAt = 0;  // 读回校验失败的偏移（写成功但数据不对）
  uint32_t ms = 0;
};

// paceMs>0 = 节流写：每写一块停一会再写下一块（判别供电塌陷 vs 卡本身，
// 见 stage F）。Arduino 预处理器生成的原型不带默认参数，所以每个调用点都显式传。
static StageRes runStage(const char *name, size_t chunk, size_t total, uint32_t paceMs) {
  StageRes r;
  SD_MMC.remove("/wtest.bin");
  File f = SD_MMC.open("/wtest.bin", FILE_WRITE);
  if (!f) {
    r.fail = r.openFail = true;
    Serial.printf("  ✖ %s：开文件失败\n", name);
    printDrvErr("   ");
    return r;
  }
  uint32_t t0 = millis();
  unsigned long done = 0;
  bool progLine = false;
  errClear();
  while (done < total) {
    fillPattern(g_buf, chunk, done);
    size_t n = f.write(g_buf, chunk);   // 失败时返回 < chunk，IDF 错误码进钩子
    if (n != chunk) { r.fail = true; break; }
    done += n;
    if (paceMs) delay(paceMs);
    // 每 64KB 打一次进度：卡在某处死住时（本轮出现过单次调用卡 181 秒）
    // 新行不再往下走，一眼看出"挂在哪"而不是以为程序死了
    if ((done & 0xFFFFul) == 0) { Serial.printf("\r    写到 %lu KB...", done / 1024); progLine = true; }
  }
  f.close();
  r.done = done;
  r.ms = millis() - t0;
  if (progLine) Serial.println();
  if (r.fail) {
    Serial.printf("  ✖ %s：写到 %lu/%lu 字节失败（耗时 %u ms）\n",
                  name, done, total, r.ms);
    printDrvErr("   ");
    return r;
  }

  // 读回校验：写得进 ≠ 写得对（CRC/信号边际可能产生静默错字节）
  File rd = SD_MMC.open("/wtest.bin", FILE_READ);
  if (!rd) {
    r.fail = r.openFail = true;
    Serial.printf("  ✖ %s：写成功但读回打不开（已锁卡）\n", name);
    printDrvErr("   ");
    return r;
  }
  unsigned long checked = 0;
  errClear();
  while (checked < total && rd.available()) {
    size_t want = (total - checked) < chunk ? (total - checked) : chunk;
    int n = rd.read(g_buf, want);
    if (n != (int)want) { r.fail = true; break; }
    if (!checkPattern(g_buf, want, checked)) { r.fail = true; r.verifyAt = checked; break; }
    checked += n;
    if ((checked & 0xFFFFul) == 0) { Serial.printf("\r    校验到 %lu KB...", checked / 1024); progLine = true; }
  }
  rd.close();
  if (progLine) Serial.println();
  if (r.fail) {
    Serial.printf("  ✖ %s：读回校验失败 @偏移 %lu（写成功但数据不对）\n",
                  name, r.verifyAt ? r.verifyAt : checked);
    printDrvErr("   ");
    return r;
  }
  float wMBs = r.ms ? (total / 1048576.0) / (r.ms / 1000.0) : 0.0;
  Serial.printf("  ✔ %s：写 %lu KB / %u ms = %.2f MB/s，读回校验通过%s\n",
                name, total / 1024, r.ms, wMBs, paceMs ? "（含节流间隔，速度无参考意义）" : "");
  return r;
}

// ============ 一轮完整测试（一个频率） ============
struct PassRes {
  bool ran = false;            // 这一轮到底跑没跑（区别"未跑"和"挂载失败"）
  bool mounted = false;
  int readOk = -1;             // 1 过 / 0 挂 / -1 没跑
  unsigned long maxWriteOk = 0;
  bool writeFail = false;
  const char *failStage = nullptr;
  unsigned long failAt = 0;
  int failErr = 0;
  bool dead = false;           // 恢复失败，本频率余下阶段作废
  bool saw0x109 = false;       // 本轮是否出现过 CRC 错（坑18 指纹）
  bool pacedRan = false;       // F 节流写档是否跑过
  bool pacedOk = false;
  float writeMBs = 0;
  float bigMBs = 0;            // G 32KB 大块吞吐
};
static PassRes g_pass[2];
static const uint32_t kFreqs[] = {20000, 10000};

static void runPass(int idx) {
  const uint32_t freq = kFreqs[idx];
  PassRes &p = g_pass[idx];
  p = PassRes();
  p.ran = true;
  Serial.printf("\n===== 第 %d 轮：1-bit SDIO @ %u kHz =====\n", idx + 1, freq);

  p.mounted = mountAt(freq, 5);
  if (!p.mounted) { Serial.println("  ✖ 挂载失败（重试 5 次）→ 本频率整轮作废"); return; }
  Serial.printf("  ✔ 挂载成功，容量 %u MB\n", (unsigned)(SD_MMC.cardSize() / 1048576));

  p.readOk = stageRead() ? 1 : 0;

  struct Stage { const char *name; size_t chunk; size_t total; };
  const Stage kStages[] = {
    {"A 小块写 2KB（512B×4）", 512,  2 * 1024},
    {"B 写 16KB（4KB/次）",    4096, 16 * 1024},
    {"C 写 256KB（4KB/次）",   4096, 256 * 1024},
    {"D 写 2MB（4KB/次）全量", 4096, 2 * 1024 * 1024},
  };
  bool anyFail = false;
  int consecOpenFail = 0;      // 连续两档连文件都开不了 = 读/FAT 路径已挂，别再烧时间
  for (const Stage &s : kStages) {
    if (p.dead) { Serial.printf("  ○ %s：跳过（卡已锁死）\n", s.name); continue; }
    StageRes r = runStage(s.name, s.chunk, s.total, /*paceMs=*/0);
    if (g_drvSaw0x109) p.saw0x109 = true;
    if (r.fail) {
      anyFail = true;
      p.writeFail = true;
      if (!p.failStage) {           // 记第一次失败：那才是根因
        p.failStage = s.name;
        p.failAt = r.done;
        p.failErr = g_drvErr;
      }
      consecOpenFail = r.openFail ? consecOpenFail + 1 : 0;
      p.dead = !recover(freq);
      if (!p.dead && consecOpenFail >= 2) {
        Serial.println("  ✖ 连续两档连文件都开不了 = 卡不可用，本频率余下阶段作废（省时间）");
        p.dead = true;
      }
    } else {
      consecOpenFail = 0;
      p.maxWriteOk = s.total;
      if (s.total >= 256 * 1024) p.writeMBs = r.ms ? (s.total / 1048576.0) / (r.ms / 1000.0) : 0;
    }
  }

  // 判别档 E：大块挂了 → 用 512B 小块顶 2MB，区分"多块/长传输边际"和"纯粹写不动"
  if (anyFail && !p.dead) {
    StageRes r = runStage("E 判别：512B 小块写 2MB", 512, 2 * 1024 * 1024, 0);
    if (g_drvSaw0x109) p.saw0x109 = true;
    if (r.fail) {
      if (!p.failStage) { p.failStage = "E 判别"; p.failAt = r.done; p.failErr = g_drvErr; }
      p.dead = !recover(freq);
    } else {
      p.maxWriteOk = 2 * 1024 * 1024;
    }
  }

  // 判别档 F：节流写（4KB 后停 200ms）。卡有喘息时间、平均电流下来。
  //   能过 512KB（连续写的 6~30 倍）→ 供电塌陷/卡内部编程时间不够 = 坑18 方向
  //   照样在几十 KB 挂 → 与节奏无关 = 卡本身或信号，换卡 + 查上拉/去耦
  if (anyFail && !p.dead) {
    p.pacedRan = true;
    StageRes r = runStage("F 节流写 512KB（4KB + 200ms 间隔）", 4096, 512 * 1024, 200);
    if (g_drvSaw0x109) p.saw0x109 = true;
    p.pacedOk = !r.fail;
    if (r.fail && !p.failStage) { p.failStage = "F 节流写"; p.failAt = r.done; p.failErr = g_drvErr; }
    if (r.fail) recover(freq);
  }

  // 吞吐档 G：全链路过了才跑，换 32KB 大块看写速上限（DESIGN §5.7 的读写专项数据）
  //   4KB/次写 0.25MB/s 而读 1.92MB/s：
  //     大块明显提速 → 瓶颈在每次调用的固定开销（FATFS/命令时延）
  //     大块仍 0.25MB/s → 卡内部编程时间（读快写慢 = 廉价卡/掉速卡特征）
  if (!anyFail && !p.dead) {
    StageRes r = runStage("G 吞吐：32KB 大块写 2MB", 32768, 2 * 1024 * 1024, 0);
    if (g_drvSaw0x109) p.saw0x109 = true;
    if (r.fail) {
      if (!p.failStage) { p.failStage = "G 吞吐"; p.failAt = r.done; p.failErr = g_drvErr; }
      p.writeFail = true;
      recover(freq);
    } else {
      p.bigMBs = r.ms ? (2 * 1024 * 1024 / 1048576.0) / (r.ms / 1000.0) : 0;
    }
  }

  SD_MMC.end();   // 每轮收尾都拆挂载：上一轮残留的"挂载着但已废"状态会污染下一轮首次挂载
}

// ============ 结论 ============
static void verdict() {
  Serial.println("\n=================================");
  Serial.println(" 结论");
  Serial.println("=================================");
  for (int i = 0; i < 2; i++) {
    PassRes &p = g_pass[i];
    if (!p.ran) {
      Serial.printf("  %u kHz：未跑（%s 全过，不必降频复测）\n", kFreqs[i],
                    i ? "上一轮" : "上一次");
      continue;
    }
    if (!p.mounted) { Serial.printf("  %u kHz：挂载失败\n", kFreqs[i]); continue; }
    Serial.printf("  %u kHz：读 %s ｜ 写最大过 %lu KB", kFreqs[i],
                  p.readOk == 1 ? "OK" : (p.readOk == 0 ? "FAIL" : "n/a"),
                  p.maxWriteOk / 1024);
    if (p.writeFail) {
      Serial.printf(" ｜ 首挂：%s @%lu 字节，0x%x (%s)%s%s\n",
                    p.failStage ? p.failStage : "?", p.failAt,
                    p.failErr, errName(p.failErr), p.dead ? "，恢复失败=锁死" : "",
                    p.saw0x109 ? "，本轮出现过 0x109 CRC" : "");
    } else {
      if (p.writeMBs) Serial.printf(" ｜ %.2f MB/s", p.writeMBs);
      if (p.bigMBs) Serial.printf("（32KB 大块 %.2f MB/s）", p.bigMBs);
      Serial.println();
    }
    if (p.pacedRan)
      Serial.printf("    节流写（4KB+200ms）512KB：%s\n",
                    p.pacedOk ? "✔ 过 —— 停顿能救 = 供电/卡编程时间问题" : "✖ 也挂 —— 与写节奏无关");
  }
  Serial.println("---- 判读 ----");
  const PassRes &a = g_pass[0], &b = g_pass[1];
  if (!a.writeFail) {
    Serial.println("  20MHz 全过 = 写路径硬件 OK，可回填 DESIGN §5.7 待补项。");
  } else if (b.mounted && !b.writeFail) {
    Serial.println("  仅 10MHz 过 = 信号边际 → 查 WIRING §4：卡座 VDD 10µF+100nF 就近去耦、CMD/D0 实体 47k 上拉。");
  } else if (a.saw0x109 || b.saw0x109) {
    Serial.println("  出现过 0x109 CRC = DESIGN 坑 18 指纹 → 换卡排除 + 卡座去耦，断电重插再测。");
  } else if (a.pacedRan && a.pacedOk) {
    Serial.println("  连续写挂、节流写过 = 供电塌陷/卡内部编程时间不够（坑 18 方向）→ 卡座 VDD 去耦优先，其次换卡。");
  } else if (b.mounted && a.writeFail && b.writeFail) {
    Serial.println("  降频 10MHz 无改善 = 排除 20MHz 信号完整性 → 剩两个嫌疑：卡座供电去耦、卡本身；换卡最快定案。");
  } else {
    Serial.println("  写路径仍挂 → 按上面打印的驱动层码继续查（0x107=信号/上拉/供电，0x109=CRC/去耦）。");
  }
}

static void runAll() {
  Serial.printf("\n\n############ 测试开始 %lu ms ############\n", (unsigned long)millis());
  runPass(0);
  if (g_pass[0].writeFail) runPass(1);   // 20MHz 有挂才降频复测
  else g_pass[1] = PassRes();
  verdict();
  Serial.println("\n[发 `r` 重跑（换卡/重插后用）]");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  esp_log_set_vprintf(hookVprintf);   // ① 截驱动层返回码（打印行为不变）

  Serial.println("\n=================================");
  Serial.println(" ESP32-S3 1-bit SDIO 读写测试 v3 ");
  Serial.println("=================================");

  if (!allocBuf()) { Serial.println("❌ 内存分配失败！"); return; }
  runAll();
}

void loop() {
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'r' || c == 'R') {
      if (allocBuf()) runAll();
    }
  }
  delay(20);
}
