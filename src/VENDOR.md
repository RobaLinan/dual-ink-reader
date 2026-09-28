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
