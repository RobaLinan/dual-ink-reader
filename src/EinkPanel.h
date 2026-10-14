#ifndef EINK_PANEL_H
#define EINK_PANEL_H

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>
#include <stddef.h>

// UC8179 面板驱动封装（GDEY0583T81, 648x480, 1bpp, 4-wire SPI）
//
// 与 vendor 示例（legacy/EPD.cpp）的区别：
//   1. CS/DC/RST/BUSY 全部构造参数化 —— 一块主控可实例化左右两块屏
//   2. waitIdle() 带超时 + 喂狗 —— vendor 的 lcd_chkstatus() 是无超时死循环
//   3. showDiff() 的 DTM1(0x10) 写"真实上一帧"而非全 0xFF —— vendor 每次灌全白，
//      等于控制器以为屏幕本来是白的，该擦除的像素不被驱动，这是残影的根源
//   4. 三档速度显式化：FULL(3s) / FAST(1.5s) / PARTIAL(0.3s)
//      本质 = 0x50(CDI) 数据 + 0xE5 强制温度寄存器（假装高温换取快波形）
//
// BUSY 极性：此模组高电平 = 空闲（vendor 注释 "=0 BUSY"）。
//
// === 页面方向：短边为横边的竖版页（DESIGN §5.5）===
// 屏是横的 648×480，但排版按竖版页 PAGE_W(480) × PAGE_H(648) 走。所有绘制都
// 经过 GUI_Paint 的 Paint_SetPixel，而它自带旋转 —— 只要在 Paint_NewImage 时
// 给 PAGE_ROTATE，逻辑坐标 (0..479, 0..647) 就逐像素映射进 648×480 显存，
// 零额外缓冲、零旋转拷贝；差分/DTM1/R_MIRRORED 都在显存坐标上做，不受影响。
//   90  = 页顶朝屏右边（阅读时把设备逆时针转 90°）← 当前
//   270 = 页顶朝屏左边（顺时针转 90°）
// 编译期想翻方向：改这里，或 build 时 -DPAGE_ROTATE=270
#ifndef PAGE_ROTATE
#define PAGE_ROTATE 90
#endif
// SPI 时钟。原跑 10MHz，左屏间歇 BUSY 超时疑似链路信号完整性边界问题
// （降频后若稳定 = 诱因在此）。编译期可用 -DEINK_SPI_HZ=10000000 切回 10MHz
#ifndef EINK_SPI_HZ
#define EINK_SPI_HZ 4000000
#endif

class EinkPanel {
public:
  enum Speed : uint8_t { FULL = 0, FAST = 1, PARTIAL = 2 };

  static const int WIDTH = 648;
  static const int HEIGHT = 480;
  static const uint32_t FULL_BYTES = (uint32_t)WIDTH * HEIGHT / 8;  // 38880

  // 排版用的逻辑页（竖版：短边 480 为横边、长边 648 为竖边），
  // 由 PAGE_ROTATE 映射进上面的显存 —— 画布尺寸、行宽、页底全按这两个走
  static const int PAGE_W = HEIGHT;   // 480：行宽方向
  static const int PAGE_H = WIDTH;    // 648：翻页方向

  // mirrored: 屏物理旋转 180° 安装时置 true（对折书的右屏），
  // 流式发送时自动做 字节序反转 + 位反转
  EinkPanel(int8_t cs, int8_t dc, int8_t rst, int8_t busy,
            int8_t sck = -1, int8_t mosi = -1, bool mirrored = false);

  // SPI 初始化 + 硬件复位 + 上电(PON)。返回 false = BUSY 超时（先查接线）
  bool begin();

  // 配置刷新速度寄存器，需在 idle 时调用。之后接 showDiff()/clearWhite()
  void setSpeed(Speed s);

  // 差分刷新：DTM1 <- prev（上一帧，nullptr = 视为全白），DTM2 <- next，
  // trigger 后可选等待完成。返回 false = BUSY 超时
  bool showDiff(const uint8_t* prev, const uint8_t* next,
                bool wait = true, uint32_t timeoutMs = 8000);

  // 只灌帧不触发（DTM1+DTM2）。给 DualEink 重叠刷新用：
  // 灌完左屏立刻 trigger，再趁左屏 busy 给右屏 loadFrame
  void loadFrame(const uint8_t* prev, const uint8_t* next);

  // 整屏刷白（含旧数据也写白，相当于一次清屏全刷）
  bool clearWhite(Speed s, uint32_t timeoutMs = 8000);

  // 触发刷新 DRF(0x12)，不等待 —— P2 双屏重叠刷新时用来交错两屏的 busy 窗口
  bool trigger();
  // 等空闲。true = 正常完成，false = 超时
  bool waitIdle(uint32_t timeoutMs);

  // DSLP 深睡：图像保持，但 VCI 必须持续供电（约 1µA）。
  // 之后要用 wake() 唤醒，不能直接用 showDiff()
  void sleep();
  // 深睡后唤醒 = 硬件复位 + 重新上电
  bool wake();

  bool idle() { return digitalRead(m_busy) == HIGH; }

private:
  void reset();
  bool powerOn();
  void writeCmd(uint8_t c);
  void writeData(uint8_t d);
  void writeDataBurst(const uint8_t* data, size_t len);
  void writeRepeated(uint8_t val, size_t len);
  void streamFrame(const uint8_t* fb);  // 处理 mirrored
  static uint8_t reverseBits(uint8_t b);

  static const size_t STAGE_SIZE = 2048;  // SPI DMA 暂存区（内部 RAM）
  uint8_t* m_stage = nullptr;

  int8_t m_cs, m_dc, m_rst, m_busy, m_sck, m_mosi;
  bool m_mirrored;
  static bool s_spiBegun;  // SPI 是全局单例，多屏实例只初始化一次
};

#endif  // EINK_PANEL_H
