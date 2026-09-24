#include "FS.h"
#include "SD_MMC.h"

#define SD_MMC_CLK 12
#define SD_MMC_CMD 11
#define SD_MMC_D0  10

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=================================");
  Serial.println(" ESP32-S3 1-bit SDIO 真实测速 ");
  Serial.println("=================================");

  pinMode(SD_MMC_CMD, INPUT_PULLUP);
  pinMode(SD_MMC_D0,  INPUT_PULLUP);

  // 设置 1-bit SDIO 引脚
  if (!SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0)) {
    Serial.println("❌ 引脚配置失败！");
    return;
  }

  // 1-bit 模式下线少抗干扰强，可以稳跑 20MHz (20000kHz)
  // 第二个参数 true 表示开启 1-bit 模式
  if (!SD_MMC.begin("/sdcard", true, false, 20000)) {
    Serial.println("❌ 挂载失败！请检查 SD 卡是否已重新拔插上电。");
    return;
  }

  Serial.println("✔ 1-bit SDIO 20MHz 挂载成功！");

  // 分配 4KB 写入缓冲区
  const size_t bufSize = 4 * 1024; // 4 KB
  uint8_t *buf = (uint8_t *)malloc(bufSize);
  if (!buf) {
    Serial.println("❌ 内存分配失败！");
    return;
  }
  memset(buf, 0x55, bufSize);

  // 写入 2MB 数据进行真实测速
  const size_t totalSize = 2 * 1024 * 1024; // 2 MB
  File file = SD_MMC.open("/1bit_speed_test.bin", FILE_WRITE);
  
  if (file) {
    Serial.println("🚀 开始连续写入 2MB 数据...");
    size_t writtenTotal = 0;
    bool writeErr = false;
    uint32_t startMs = millis();

    while (writtenTotal < totalSize) {
      size_t written = file.write(buf, bufSize);
      if (written != bufSize) {
        Serial.printf("\n❌ 写入出错！期望 %d 字节，实际写入 %d 字节\n", bufSize, written);
        writeErr = true;
        break;
      }
      writtenTotal += written;
    }
    
    file.close();
    uint32_t durationMs = millis() - startMs;

    if (!writeErr && durationMs > 0) {
      float speedMBs = (writtenTotal / (1024.0 * 1024.0)) / (durationMs / 1000.0);
      Serial.printf("\n🎉 1-bit SDIO 测速成功！\n");
      Serial.printf("⏱ 实际写入: %u KB，耗时: %u ms\n", writtenTotal / 1024, durationMs);
      Serial.printf("⚡ 真实写入速度: %.2f MB/s\n", speedMBs);
    }
  } else {
    Serial.println("❌ 打开文件失败！");
  }

  free(buf);
}

void loop() {
}