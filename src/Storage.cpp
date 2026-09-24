#include "Storage.h"
#include "SD_MMC.h"

static bool s_mounted = false;

// 单次挂载尝试：上拉兜底 → setPins → begin → 根目录探针。
// 探针防"半成功"挂载：冷上电实测出现过 begin 返回 true 但紧跟着开文件失败
// （卡刚初始化完还在忙 / 初始化其实没走完），打开 "/" 失败就当本次失败处理。
static bool mountOnce() {
  // CMD/D0 内部上拉兜底：SD 规范要求 CMD/DAT 有 10-100k 上拉，模块没焊时
  // CMD 浮空 → 初始化 0x107（卡无应答）。每次重试都重新拉：end() 会拆掉
  // 引脚配置，只在 begin 前拉一次的话重试时是浮空的
  pinMode(39, INPUT_PULLUP);  // CMD
  pinMode(40, INPUT_PULLUP);  // D0

  // 正式引脚（DESIGN §1.1）。第三个参数 false = 不用 CARD_D1/D2/D3（1-bit 模式）
  if (!SD_MMC.setPins(/*clk=*/38, /*cmd=*/39, /*d0=*/40)) return false;

  // 1-bit 模式线少抗干扰强，稳跑 20MHz（§5.7 实测定案；4-bit 在 core 2.0.17 + S3 不稳）
  if (!SD_MMC.begin("/sdcard", /*mode1bit=*/true, /*format_if_fail=*/false,
                    /*frequency=*/20000))
    return false;

  File probe = SD_MMC.open("/");
  if (!probe) { SD_MMC.end(); return false; }
  probe.close();
  return true;
}

bool Storage::begin() {
  if (s_mounted) return true;

  // 冷上电首挂偶发 0x107（卡与双屏共用 3V3 轨，上电爬升慢、卡 POR 就绪晚；
  // 复位键不断卡的电所以复位后首挂即成。2026-09-24 实测：固定 100/200ms 的
  // 3 次重试覆盖不住最坏就绪窗口，连挂 9 次全败）。改指数退避 5 次，
  // 最坏多等 ~3s 换来冷上电基本必挂上；正常情况（含深睡唤醒）首次即成、零等待。
  static const uint16_t kBackoffMs[] = {200, 400, 800, 1600};
  const int kAttempts = 5;
  for (int attempt = 0; attempt < kAttempts; attempt++) {
    if (attempt) {
      SD_MMC.end();                       // 拆掉半初始化状态
      delay(kBackoffMs[attempt - 1]);     // 给卡稳定时间（递增）
    }
    if (mountOnce()) {
      s_mounted = true;
      return true;
    }
    Serial.printf("[SD] mount failed (attempt %d/%d)\n", attempt + 1, kAttempts);
  }
  Serial.println("[SD] mount failed - check card/wiring");
  return false;
}

bool Storage::mounted() { return s_mounted; }
