#ifndef DBG_LOG_H
#define DBG_LOG_H

#include <Arduino.h>

// 串口日志镜像 + PSRAM 环形缓冲（DESIGN §11 网页调试台 /debug）：
//   include 本头文件后，本编译单元里所有 Serial.xxx 都被宏重定向到 DbgSerial：
//   一份原样转发真串口（本地串口工具照旧；Serial 是 HardwareSerial 还是
//   HWCDC 随 ARDUINO_USB_CDC_ON_BOOT 变，代理只用 Stream 接口），一份带 [ms] 行
//   时间戳进 PSRAM 环形缓冲（32KB）—— /debug 页增量拉取，屏幕挂了也能远程看串口尾巴。
// 规则：
//   · 本头文件放**最后一条 #include**（库头先展开，宏只影响之后的本文件代码；
//     库自己的 .cpp 是独立编译单元，不经宏，行为不变）
//   · DbgLog.cpp 在宏生效前抓真 UART 指针，转发不会自递归
//   · 环形缓冲非中断安全：ISR 里别用 Serial（本来也不该）

class DbgSerialProxy : public Stream {
public:
  DbgSerialProxy();

  // ---- Arduino 串口惯用面：转发真 UART + 镜像进环形缓冲 ----
  void begin(unsigned long baud = 115200);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t n) override;
  using Print::write;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  explicit operator bool() const { return true; }   // if (Serial) 恒真
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // ---- /debug 页访问 ----
  // since = 客户端游标（累计字节序号）；拷出 since 之后的可读部分（原样字节，
  // 客户端 textContent 展示，不转义），next = 新游标（下次传回）。
  // 单次最多 8KB（超了从头裁掉，打"已滚出"标记）；游标失效（缓冲滚过/超前）
  // 自动落到最旧可用
  void dump(String& out, uint32_t since, uint32_t& next);
  uint32_t seq() const { return m_seq; }

private:
  void mirror(uint8_t b);

  Stream* m_uart;           // 真串口（HardwareSerial 或 HWCDC，DbgLog.cpp 静态初始化期抓的 &Serial）
  uint8_t* m_ring;          // PSRAM 环形缓冲（懒分配）
  size_t m_cap;
  size_t m_head;            // 写位置 = m_seq % m_cap
  uint32_t m_seq;           // 累计镜像字节数（含注入的时间戳）
  bool m_lineStart;
};

extern DbgSerialProxy DbgSerial;

// 本文件放最后一条 include 就是为了这行：之后本编译单元的 Serial.* 全走代理
#define Serial DbgSerial

#endif  // DBG_LOG_H
