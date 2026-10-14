# 双屏对折电子书

基于 ESP32-S3 的双墨水屏对折电子书阅读器，形似书本，展开后左右各一块 5.83" 墨水屏用于对页显示。

## 硬件

| 组件 | 型号/规格 |
|---|---|
| 主控 | ESP32-S3 N16R8（16MB Flash / 8MB PSRAM） |
| 墨水屏 | GDEY0583T81 ×2，648×480，UC8179 控制器，138dpi |
| 存储 | microSD（SDMMC 1-bit 模式） |
| 唤醒 | DRV5032 霍尔传感器 + 磁铁（合盖深睡 / 开盖唤醒） |
| 按键 | 静音微动 ×4（Prev / Next / Menu / Font） |
| 电源 | 5V 充电电池包（须支持常开，深睡 µA 级不被切断） |

三档刷新：全刷 3.7s / 快刷 1.2s / 局部 ~0.3s。

## 功能

- **EPUB 阅读**：zip 流式解压（miniz 迭代式 API）+ XHTML 解析（tinyxml2，PSRAM 池）+ 图片解码（TJpgDec / PNGdec）
- **TXT 阅读**：UTF-8 排版，CJK 避头悬挂 + 词断
- **双屏对页**：左灌帧 → DRF → 趁左 busy 给右灌帧 → DRF，重叠刷新 ~max+60ms
- **SD 卡书库**：FAT 分区 9MB，支持 EPUB/TXT 文件浏览与删除
- **书签 / 目录 / 进度**：NVS 持久化，主页显示阅读进度
- **WiFi 调试台**：SoftAP 配网 + OTA + `/debug` 网页远程诊断
- **多字体**：楷体/宋体/黑体/衬线/无衬线，三档字号（16/24/32px），字体包预渲染 bin 格式
- **深睡唤醒**：霍尔 EXT1 唤醒，屏 VCI 不断电，唤醒直接差分刷新

## 项目结构

```
eink.ino              # Arduino 主 sketch（setup/loop + 状态机）
src/                  # 源码模块
  EinkPanel.*         # 单屏 SPI 驱动（构造参数化，支持 mirrored）
  DualEink.*          # 双屏重叠刷新
  EpubBook.*          # EPUB 解析与排版
  EpubZip.*           # miniz 迭代式 zip 解压
  EpubImg.*           # JPEG/PNG 插图解码与绘制
  TxtBook.*           # TXT 排版（CJK 避头悬挂 + token 推挤）
  CjkFont.*           # CJK 字库绘制（预渲染 bin 格式）
  Storage.*           # SD 卡管理（SDMMC 1-bit）
  HomePage.*          # 选书主页
  ListPage.*          # 文件列表
  Bookmarks.*         # 书签管理
  Keys.*              # 按键事件模型
  SysMenu.*           # 系统菜单
  NetSvc.*            # WiFi / OTA / 调试台
  UiDraw.*            # 通用 UI 绘制工具
  DbgLog.*            # 调试日志
  miniz/              # vendored miniz 3.0.2（MIT）
  tinyxml2/           # vendored tinyxml2 10.0.0（Zlib + PSRAM 补丁）
  tjpgd/              # vendored TJpgDec R0.03（ChaN 许可）
assets/
  fonts/              # 预渲染字体包（.bin）
  books/              # 示例书籍（公版）
CAD/                  # FreeCAD 外壳模型
PCB/                  # EasyEDA Pro 原理图
tools/                # 字体打包 / EPUB 工具脚本
hall_sleep_test/      # 霍尔深睡测试
key_test/             # 按键电气测试
sdcard/               # SD 卡底层写测试
legacy/               # 旧版 EPD 驱动（已废弃，存档）
```

## 编译

Arduino IDE 或命令行（ESP32 Core 2.0.17）：

- **FQBN**：`esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc`
- **分区表**：以 sketch 根目录 `partitions.csv` 为准（app 3MB / FAT 9MB / 16MB 总）
- **依赖**：PNGdec 库（`~/Arduino/libraries/PNGdec`，未 vendor）
- **串口**：115200

## 接线

详见 [WIRING.md](WIRING.md)。关键引脚：

- SPI：SCK 12 / MOSI 11（两屏并联）
- 左屏：CS 14 / DC 15 / RST 16 / BUSY 17
- 右屏：CS 18 / DC 21 / RST 47 / BUSY 48（经铰链排线）
- SD：CLK 38 / CMD 39 / D0 40（SDMMC 1-bit）
- 按键：PREV 1 / NEXT 2 / MENU 9 / FONT 10
- 霍尔：GPIO 4

## 文档

- [DESIGN.md](DESIGN.md) — 完整设计文档（引脚、驱动、排版、踩坑记录）
- [WIRING.md](WIRING.md) — 接线表（PCB 制图用）
- [BOM.md](BOM.md) — 备料清单

## 许可

本项目源码仅供个人学习。Vendored 第三方库各自遵循原始许可（MIT / Zlib / ChaN）。
