#include <Arduino.h>
#include <stdarg.h>

// 宏生效前抓真串口指针（见 DbgLog.h 头注释）：必须先拿到 &Serial 再 include
// DbgLog.h —— 之后本文件里的 "Serial" 都已被宏替换，不会自递归转发。
// 用 auto 捕获具体类型：Serial 随 ARDUINO_USB_CDC_ON_BOOT 是 HardwareSerial
// 或 HWCDC（两者都派生自 Stream）；begin() 只有具体类型有，故走这个指针。
static auto* const s_realUart = &Serial;

#include "DbgLog.h"

DbgSerialProxy DbgSerial;

static const size_t kRingSize = 32768;   // PSRAM 环形缓冲 ≈ 最近几百行日志
static const uint32_t kDumpMax = 8192;   // /debug/log 单次最大返回字节

DbgSerialProxy::DbgSerialProxy()
    : m_uart(s_realUart), m_ring(nullptr), m_cap(0), m_head(0), m_seq(0),
      m_lineStart(true) {}

void DbgSerialProxy::begin(unsigned long baud) { s_realUart->begin(baud); }

size_t DbgSerialProxy::write(uint8_t b) {
  m_uart->write(b);
  mirror(b);
  return 1;
}

size_t DbgSerialProxy::write(const uint8_t* buf, size_t n) {
  m_uart->write(buf, n);
  for (size_t i = 0; i < n; i++) mirror(buf[i]);
  return n;
}

int DbgSerialProxy::available() { return m_uart->available(); }
int DbgSerialProxy::read() { return m_uart->read(); }
int DbgSerialProxy::peek() { return m_uart->peek(); }
void DbgSerialProxy::flush() { m_uart->flush(); }

// 真 UART 的 printf 也走这里（vsnprintf 上限 512B/行，观测最长 ~150B，足够）
int DbgSerialProxy::printf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return 0;
  size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
  write((const uint8_t*)buf, len);
  return n;
}

void DbgSerialProxy::mirror(uint8_t b) {
  if (!m_ring && !(m_ring = (uint8_t*)ps_malloc(kRingSize))) return;  // PSRAM 没了就只留真 UART
  if (!m_cap) m_cap = kRingSize;
  if (m_lineStart && b != '\n') {   // 行首注入 [ms] 时间戳（只进缓冲不进 UART）
    char ts[16];
    int k = snprintf(ts, sizeof(ts), "[%lu] ", (unsigned long)millis());
    for (int i = 0; i < k; i++) {
      m_ring[m_head] = (uint8_t)ts[i];
      m_head = (m_head + 1) % m_cap;
      m_seq++;
    }
    m_lineStart = false;
  }
  m_ring[m_head] = b;
  m_head = (m_head + 1) % m_cap;
  m_seq++;
  if (b == '\n') m_lineStart = true;
}

void DbgSerialProxy::dump(String& out, uint32_t since, uint32_t& next) {
  out = "";
  if (!m_ring || !m_cap) { next = m_seq; return; }
  uint32_t oldest = m_seq - (m_seq >= (uint32_t)m_cap ? (uint32_t)m_cap : m_seq);
  if (since > m_seq || since < oldest) since = oldest;   // 游标失效 → 最旧可用
  bool clipped = false;
  if (m_seq - since > kDumpMax) { since = m_seq - kDumpMax; clipped = true; }
  uint32_t n = m_seq - since;
  out.reserve(n + 48);
  if (clipped) out += F("……（更早日志已滚出环形缓冲）……\n");
  uint32_t off = since % m_cap;   // 环形拷贝（最多绕回一次），NUL 跳过（String 装不下）
  for (uint32_t i = 0; i < n; i++) {
    uint8_t c = m_ring[(off + i) % m_cap];
    if (c) out += (char)c;
  }
  next = m_seq;
}
