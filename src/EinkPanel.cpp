#include "EinkPanel.h"

// UC8179 命令（取自 GDEY0583T81 数据手册 §6 与 vendor 示例）
static const uint8_t CMD_PSR = 0x00;   // Panel Setting
static const uint8_t CMD_PON = 0x04;   // Power On
static const uint8_t CMD_POF = 0x02;   // Power Off
static const uint8_t CMD_DSLP = 0x07;  // Deep Sleep
static const uint8_t CMD_DTM1 = 0x10;  // 旧数据 (KW/old pixel data)
static const uint8_t CMD_DTM2 = 0x13;  // 新数据 (new pixel data)
static const uint8_t CMD_DRF = 0x12;   // Display Refresh
static const uint8_t CMD_CDI = 0x50;   // VCOM and Data Interval Setting
static const uint8_t CMD_CCSET = 0xE0; // Cascade Setting
static const uint8_t CMD_TSSET = 0xE5; // Force Temperature（vendor 用它换速度档位）

bool EinkPanel::s_spiBegun = false;

EinkPanel::EinkPanel(int8_t cs, int8_t dc, int8_t rst, int8_t busy,
                     int8_t sck, int8_t mosi, bool mirrored)
    : m_cs(cs), m_dc(dc), m_rst(rst), m_busy(busy),
      m_sck(sck), m_mosi(mosi), m_mirrored(mirrored) {}

bool EinkPanel::begin() {
  if (!s_spiBegun) {
    if (m_sck >= 0 && m_mosi >= 0) {
      SPI.begin(m_sck, -1, m_mosi, -1);
    } else {
      SPI.begin();
    }
    s_spiBegun = true;
  }
  // 上拉：面板未上电/复位失败时 BUSY 是浮空线，裸 INPUT 会随机读到 LOW，
  // 表现为"时好时坏的 BUSY 超时"。UC8179 的 BUSY 为推挽输出，上拉无副作用
  pinMode(m_busy, INPUT_PULLUP);
  pinMode(m_rst, OUTPUT);
  pinMode(m_dc, OUTPUT);
  pinMode(m_cs, OUTPUT);
  digitalWrite(m_cs, HIGH);
  digitalWrite(m_dc, LOW);
  digitalWrite(m_rst, HIGH);

  // SPI DMA 只能访问内部 RAM，PSRAM 里的帧缓冲必须先拷进暂存区再发
  m_stage = (uint8_t*)malloc(STAGE_SIZE);

  reset();
  if (!powerOn()) return false;
  setSpeed(FULL);
  return true;
}

void EinkPanel::reset() {
  digitalWrite(m_rst, LOW);
  delay(10);  // 数据手册：至少 10ms
  digitalWrite(m_rst, HIGH);
  delay(10);
}

bool EinkPanel::powerOn() {
  writeCmd(CMD_PSR);
  writeData(0x1F);  // 与 vendor 一致；bit4 SHD_N=1 保持 booster 开启
  writeCmd(CMD_PON);
  delay(300);       // 数据手册：PON 后内部电源建立需要时间
  return waitIdle(5000);
}

void EinkPanel::setSpeed(Speed s) {
  switch (s) {
    case FULL:
      // E0 必须写回 0x00：TSFIX=0 → 温度取内置传感器（复位默认值）。
      // 快刷/局部刷把 E0 置 0x02（TSFIX=1，温度用 E5 强制值），不恢复的话
      // "全刷"会沿用强制温度的快波形 → 清不干净残影。E5 此时无效，无需管
      writeCmd(CMD_CCSET);
      writeData(0x00);
      writeCmd(CMD_CDI);
      writeData(0x21);
      writeData(0x07);
      break;
    case FAST:
      writeCmd(CMD_CDI);
      writeData(0x29);
      writeData(0x07);
      writeCmd(CMD_CCSET); writeData(0x02);
      writeCmd(CMD_TSSET); writeData(0x5A);  // 0x5A → 1.5s 快刷波形
      break;
    case PARTIAL:
      writeCmd(CMD_CDI);
      writeData(0xA9);
      writeData(0x07);
      writeCmd(CMD_CCSET); writeData(0x02);
      writeCmd(CMD_TSSET); writeData(0x6E);  // 0x6E → 局部刷波形
      break;
  }
}

void EinkPanel::loadFrame(const uint8_t* prev, const uint8_t* next) {
  writeCmd(CMD_DTM1);
  if (prev) {
    streamFrame(prev);
  } else {
    writeRepeated(0xFF, FULL_BYTES);  // 无历史帧时按"上一帧全白"处理
  }
  writeCmd(CMD_DTM2);
  streamFrame(next);
}

bool EinkPanel::showDiff(const uint8_t* prev, const uint8_t* next,
                         bool wait, uint32_t timeoutMs) {
  loadFrame(prev, next);
  trigger();
  if (wait) return waitIdle(timeoutMs);
  return true;
}

bool EinkPanel::clearWhite(Speed s, uint32_t timeoutMs) {
  setSpeed(s);
  writeCmd(CMD_DTM1);
  writeRepeated(0xFF, FULL_BYTES);
  writeCmd(CMD_DTM2);
  writeRepeated(0xFF, FULL_BYTES);
  trigger();
  return waitIdle(timeoutMs);
}

bool EinkPanel::trigger() {
  writeCmd(CMD_DRF);
  delay(1);  // 数据手册：DRF 后至少 200µs 才能发下一条命令
  return true;
}

bool EinkPanel::waitIdle(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(m_busy) != HIGH) {  // 高电平 = 空闲
    if (millis() - start > timeoutMs) return false;
    delay(1);  // 喂看门狗 + 让出调度
  }
  return true;
}

void EinkPanel::sleep() {
  writeCmd(CMD_CDI);
  writeData(0xF7);
  writeCmd(CMD_POF);
  waitIdle(2000);
  delay(100);  // 数据手册：POF 后至少 200µs
  writeCmd(CMD_DSLP);
  writeData(0xA5);  // DSLP 校验码
}

bool EinkPanel::wake() {
  reset();
  return powerOn();
}

void EinkPanel::writeCmd(uint8_t c) {
  SPI.beginTransaction(SPISettings(EINK_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(m_cs, LOW);
  digitalWrite(m_dc, LOW);
  SPI.transfer(c);
  digitalWrite(m_cs, HIGH);
  SPI.endTransaction();
}

void EinkPanel::writeData(uint8_t d) {
  SPI.beginTransaction(SPISettings(EINK_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(m_cs, LOW);
  digitalWrite(m_dc, HIGH);
  SPI.transfer(d);
  digitalWrite(m_cs, HIGH);
  SPI.endTransaction();
}

void EinkPanel::writeDataBurst(const uint8_t* data, size_t len) {
  SPI.beginTransaction(SPISettings(EINK_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(m_cs, LOW);
  digitalWrite(m_dc, HIGH);
  SPI.writeBytes(const_cast<uint8_t*>(data), len);
  digitalWrite(m_cs, HIGH);
  SPI.endTransaction();
}

void EinkPanel::writeRepeated(uint8_t val, size_t len) {
  uint8_t buf[256];
  memset(buf, val, sizeof(buf));
  while (len > 0) {
    size_t chunk = len > sizeof(buf) ? sizeof(buf) : len;
    writeDataBurst(buf, chunk);
    len -= chunk;
  }
}

uint8_t EinkPanel::reverseBits(uint8_t b) {
  b = ((b & 0x55) << 1) | ((b >> 1) & 0x55);
  b = ((b & 0x33) << 2) | ((b >> 2) & 0x33);
  return (b << 4) | (b >> 4);
}

void EinkPanel::streamFrame(const uint8_t* fb) {
  if (!m_mirrored) {
    writeDataBurst(fb, FULL_BYTES);
    return;
  }
  // 180° 旋转 = 字节序反转 + 字节内位反转。分块转换后发送，
  // 避免一次性再分配 FULL_BYTES 的镜像缓冲。
  uint8_t buf[512];
  for (uint32_t k = 0; k < FULL_BYTES; k += sizeof(buf)) {
    uint32_t n = FULL_BYTES - k > sizeof(buf) ? sizeof(buf) : FULL_BYTES - k;
    for (uint32_t i = 0; i < n; i++) {
      buf[i] = reverseBits(fb[FULL_BYTES - 1 - (k + i)]);
    }
    writeDataBurst(buf, n);
  }
}
