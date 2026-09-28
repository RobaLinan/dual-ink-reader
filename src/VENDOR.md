# Vendored 第三方库（P4 引入，锁版本）

EPUB = zip 容器（miniz 解压）+ XHTML/OPF（tinyxml2 解析）。
两个库都整体拷贝进 `src/`，随 sketch 一起编译，**不依赖 arduino-cli lib 在线解析**
（离线可复现，版本永不漂移）。

## miniz 3.0.2
- 来源：https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
- 文件：`miniz/miniz.c`、`miniz/miniz.h`（原样，无补丁）
- 许可证：MIT（见 miniz/LICENSE，仓库根）
- 用法：`EpubZip` 用 `mz_zip_reader_init` 注册 SD File 的读/seek 回调 +
  **`mz_zip_reader_extract_iter_new/read/free`（迭代式）**流式解压条目到 SD 临时文件
  （DESIGN 坑 6：>280KB 章节先落 SD 再流式处理，绝不在 MCU 上整本/整章常驻 RAM）
- **栈安全（坑 16，2026-09-24）**：不能用 `extract_to_callback`/`extract_to_mem*`——
  它们把 `tinfl_decompressor`（≈8.2KB：`m_look_up[3][1024]` 霍夫曼表 + 树/码长表）
  声明为**栈上局部变量**（miniz.c 两处 `tinfl_decompressor inflator;`），而 Arduino
  `loopTask` 默认栈只有 8KB，进函数即栈溢出（串口 `b` 切 EPUB 直接 reboot 的根因）。
  迭代式把解压器状态内嵌在 `m_pAlloc` 堆分配的 `mz_zip_reader_extract_iter_state`
  里，栈上只留 1KB 拷贝缓冲。**升级 miniz 时保持这个调用方式**（仍是零补丁）。

## tinyxml2 10.0.0
- 来源：https://github.com/leethomason/tinyxml2/archive/refs/tags/10.0.0.tar.gz
- 文件：`tinyxml2/tinyxml2.cpp`、`tinyxml2/tinyxml2.h`
- 许可证：Zlib（见源码头注释）
- **补丁（1 处，tinyxml2.h）**：定义 `TIXML2_PSRAM_POOL` 时 `MemPoolT` 的
  4KB 块改走 `ps_malloc`（失败回退 `malloc`）/`free`，代替 `new/delete`。
  原因：EPUB 大章节 DOM 节点多，池块全进 DRAM（~250KB）会 OOM；
  PSRAM 8MB 充裕。`EpubBook.cpp` 编译该库前 `#define TIXML2_PSRAM_POOL`。
  补丁位置：`tinyxml2.h` 顶部 vendor patch 注释块 + `MemPoolT::Alloc/Clear` 两处 `#if`。
  升级版本时保留补丁（冲突很小，就两行）。
- 备注：tinyxml2 不解析外部 DTD；`<!DOCTYPE ...>` 走 XMLUnknown 跳过；
  未声明实体（如 `&nbsp;`）原样透传不报错——`EpubBook` 在解析前
  预替换常见 HTML 实体（`&nbsp;`→空格等）。

## 图片解码（插图）
- **JPEG = ChaN TJpgDec R0.03（vendored 源码）**：`tjpgd/tjpgd.c` +
  `tjpgd/tjpgd.h` + `tjpgd/tjpgdcnf.h`（配置），来源
  https://github.com/Bodmer/TJpg_Decoder/tree/master/src （Bodmer 库内的
  ChaN 原版，未改动）。许可证：ChaN 宽松许可（源文件头，可商用需自负责任）。
  块回调式（16×16 RGB565 MCU），工作区 `TJPGD_WORKSPACE_SIZE`=3500B DRAM。
  **接入方两个契约（坑 21，踩过）**：
  - 输出回调的 `JRECT` 是**闭区间**：宽高 = `right-left+1`/`bottom-top+1`，
    `bitmap` 里像素紧凑排列、步进 = `right-left+1`（少 +1 = 逐行错位涂抹）。
  - Bodmer 补丁版 `jd_prepare` 会**保留**入参 `JDEC.swap`（先读后清零回填），
    字节序开关由调用方负责：不显式 `memset(&jd,0,...)` 就是拿栈垃圾当开关。
  **为什么不用 ESP32-S3 ROM 的 `esp_rom_tjpgd_prepare/decomp`（坑 19）**：
  那是真 ROM 函数（ld `PROVIDE` 到 0x40000858/0x40000864）。首版接入后 prepare
  即 JDR_INP（2026-09-28 屏上实测），根因是 **ChaN 输入回调约定 `buf=NULL`
  表示"跳过 n 字节"**（JFIF APP0/EXIF 段跳过必走这条路）没实现——ROM 与源码
  同此约定。ROM 无从调试，弃用改 vendor ChaN R0.03，行为可控可查。
- **PNG = PNGdec 库（Larry Bank，BSD）**：`~/Arduino/libraries/PNGdec`
  （arduino-cli 本机已装，**未 vendor**——若换机器编译需先装）。
  行回调式；`PNG` 对象 ≈35KB（32KB inflate 窗口）必须**堆分配**，
  栈上 `new PNG` 会爆 8KB loopTask 栈（坑 16 同因）。隔行 PNG 不支持（跳过）。
  **已知缺陷（坑 21，库未修）**：`PNGRGB565` 调色板路径按 `x+=2`（4bpp）/
  `x+=4`（2bpp）整组步进，**奇数宽度会多写 1~3 个像素** → 调用方行缓冲必须
  按 `(width+4)*2` 分配吸收越界（`EpubImg::pngDrawCb` 就是这么做的，否则
  写穿 malloc 尾部魔数 → `CORRUPT HEAP` 重启）。
- 两者都只需块回调，峰值 RAM 与图片分辨率无关；`src/EpubImg.cpp` 封装
  probe（读文件头量尺寸）/draw（等比缩放 + 4×4 Bayer 抖动 → Paint 画布）。
