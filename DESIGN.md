# 双屏对折电子书 · 设计文档（方案 v4）

> 参考项目：The Open Book（单屏，字库/分页经验）、atomic14 diy-esp32-epub-reader（软件分层）、**Diptyx**（双 5.83" 对折，硬件形态几乎相同，MIT）。
> 屏幕：GDEY0583T81 ×2，648×480，UC8179 控制器，138dpi，带转接板模块。
> 全刷 3s / 快刷 1.5s / 局部 0.3s（数据手册 §7.2）。

## 1. 引脚分配

### 1.1 ESP32-S3 N16R8（目标板）
```
墨水屏（SPI2，两屏共用 SCK/MOSI）：
  SCK 12, MOSI 11
  左屏：CS 14, DC 15, RST 16, BUSY 17
  右屏：CS 18, DC 21, RST 47, BUSY 48     ← 右屏构造时 mirrored=true
SD 卡（SDMMC 1-bit，4-bit 实测不稳，见 §5.7）：
  CLK 38, CMD 39, D0 40（D1/D2/D3 不接线，41/42/13 空出备用）
按键（低有效，内部上拉，可 ext1 唤醒）：
  PREV 1, NEXT 2, MENU 9, FONT 10
霍尔 DRV5032 OUT：4    （全极性开关型；误买锁存型则唤醒逻辑改"任一沿都醒"）
电池电量：优先用电池包自带指示灯；否则 5V 分压进 GPIO 8（ADC1_CH7）
备用：5, 6, 7、41、42、13
```
避开：strapping 0/3/45/46、USB 19/20、**内部 Flash 占用的 26-32**、八线 PSRAM 占用的 33-37、UART0 43/44。
> P3 实测教训（2026-09-24）：SD CMD 误接 GPIO29 → `sdmmc_init_ocr` 0x107
> （卡无应答）。26-32 是 N16R8 内部 Flash 引脚，硬件占死，任何外设都不能用。

编译 FQBN：`esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default`

### 1.2 经典 ESP32 Dev Module（❌ 已废弃 —— 代码不再支持，仅存档）
> P3 起 sketch 只面向 ESP32-S3（`CONFIG_IDF_TARGET_ESP32S3` 分支已删）。
> 旧面包板接线：BUSY 13, RES 12, DC 14, CS 27, SCK 18, MOSI 23。

## 2. 电源
```
5V 电池包 → 开发板 5V/VIN 脚
开发板 3V3 脚 → 屏L/屏R VCI（各加 100µF+100nF 就近去耦）、SD 模块、DRV5032、按键上拉
```
**电池包选型硬指标：必须无自动关机/支持常开**（深睡整机几十 µA，普通充电宝低电流自动断电后唤不醒）。
**电流尖峰预案**：双屏重叠刷新峰值 300-400mA，若实测重启 → 5V 后加 AMS1117-3.3 模块单独喂双屏。
深睡时屏 VCI 保持供电（DSLP 约 1µA），唤醒直接差分刷新。

## 3. 驱动要点（src/EinkPanel.* + src/DualEink.*）

与 vendor 代码（legacy/EPD.cpp）的本质区别：
1. 引脚全部构造参数化 → 可实例化左右两屏
2. `waitIdle(timeout)` 带超时 + 喂狗（vendor 是无超时死循环）
3. `showDiff(prev, next)` 的 DTM1(0x10) 写**真实上一帧**（nullptr=全白）；
   vendor 每次灌 0xFF，等于控制器以为屏本是白的 → 残影根源
4. 三档速度 = 寄存器组合：
   | 档位 | 0x50 CDI | 0xE0 | 0xE5 | 实测（单屏 / 双屏重叠 L+R） |
   |---|---|---|---|---|
   | FULL | 0x21 0x07 | 0x00 | — | **3749ms / 3815ms** |
   | FAST | 0x29 0x07 | 0x02 | 0x5A | **1232ms / 1306ms** |
   | PARTIAL | 0xA9 0x07 | 0x02 | 0x6E | ~0.3s（待实测） |
   0xE5 本质是**强制温度寄存器**，靠假装高温让控制器从 OTP 挑快波形；
   开关在 **0xE0 的 TSFIX(bit1)**：1=用 E5 强制值，0=用内置温度传感器（复位默认）。
   ⚠️ `setSpeed(FULL)` 必须把 0xE0 写回 0x00，否则快刷后再全刷会沿用强制温度的
   快波形 → 清不干净残影（已修，UC8179.pdf §46/§49 确认）。
   5. `DualEink::present()`（P2）：左灌帧→DRF→趁左 busy 给右灌帧→DRF→等两屏 idle。
   对照 Diptyx `DisplayPictureBoth()`，同一形状；单屏串行 2×1.3s → 重叠 ≈max+60ms
5. `mirrored=true`：流式发送时做字节序反转+位反转（右屏物理倒装）
6. BUSY 高电平 = 空闲（此模组极性，与 vendor 注释 "=0 BUSY" 一致）
7. **页面方向（2026-09-28 改：短边为横边的竖版页）**：屏恒是 648×480 横版
   显存，排版则按竖版页 `PAGE_W`(480) × `PAGE_H`(648) 走。旋转不自己拷缓冲，
   交给 `Paint_NewImage(..., PAGE_ROTATE)` —— GUI_Paint 的 `Paint_SetPixel`
   逐像素做坐标映射（**90 = 页顶朝屏右边**，阅读时把设备逆时针转 90°；
   270 = 反向，改 `EinkPanel.h` 的 `PAGE_ROTATE` 或编译加 `-DPAGE_ROTATE=270`）。
   所有绘制（`Paint_Draw*` + `CJK.drawGlyph`）都经 `Paint_SetPixel`，所以零额外
   缓冲、零旋转拷贝；差分/DTM1/`mirrored` 仍在显存坐标上原样工作。
   ⚠️ 代价：`Paint_SetPixel` 边界判断是 `>` 不是 `≥`，**逻辑 x=480 会写穿显存**
   （旋转后 x→显存行号，SetPixel 内部拦不住）→ 三处钳位是硬需求：页眉超长
   截断（`TxtBook::drawHeader`）、行宽硬上限 464（`HARD_PX = PAGE_W-16`）、
   CJK ink 出界丢弃（`CjkFont::drawGlyph`）。离屏回归见 §6
   `tools/rot_layout_test.cpp`（哨兵带验证最坏排版零越界 + 反例确认 x=480 必穿）。

**P1 实测数据**（eink.ino demo，串口 115200，ESP32-S3 实测 2026-09-23）：
```
full refresh #0: 3749 ms (ok)
fast refresh #1: 1232 ms (ok)
fast refresh #2: 1232 ms (ok)
fast refresh #3: 1232 ms (ok)
```
⚠️ P1 首版"刷完花屏"根因：demo 漏调 `Paint_NewImage()` → GUI_Paint 全局
Width/Height/Scale=0，`Paint_Clear` 空转、所有绘制被边界检查丢弃，帧缓冲一直
是 malloc 垃圾。SPI/时序本身没问题（耗时数据是真实的）。

**P2 实测数据**（双屏 `DualEink::present()` 重叠刷新，2026-09-23）：
```
both panels OK - dual overlapping mode
full refresh #0: 3815 ms (ok)     ← 单屏 3749ms，重叠代价仅 +66ms
fast refresh #N L+R: 1306 ms (ok) ← 单屏 1232ms，重叠代价仅 +74ms（右屏灌帧 60ms 藏在左 busy 里）
100 次连刷（每 5 快插 1 全）：0 超时 0 花屏 0 重启
```
⚠️ P2 首版"上电全刷花屏"根因：setup 把页 0 画进 fbShown，而 refreshPair()
固定拿 fbDraw 当新数据 → 首帧 DTM2 = malloc 垃圾；且 setup 漏了 swapPair()。
修法 = 四缓冲先 memset 白 + 画进 draw 缓冲 + 收尾 swap（双缓冲纪律：
shown 永远 = 屏上真实内容）。
**双屏同刷电流峰值未实测**（手头无表）；100 次无重启 = 供电间接通过，
若日后出现重启按 §2 预案上 AMS1117。

## 4. 分阶段路线
| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 ✅ | 去损坏引用，旧驱动移 legacy/ | 编译通过 |
| P1 ✅ | EinkPanel 类 + 翻页 demo | 全刷/快刷实测、20 次快刷残影可接受 |
| P2 ✅ | 第二块屏 + `DualEink::present()` 重叠刷新（对照 Diptyx 固件） | 整页 ≤1.8s ✅（实测 1306ms）、100 次无花屏无重启 ✅、双屏电流待补测（无表；100 次无重启=间接通过） |
| P3 ✅ | SDMMC + UTF-8 + 字体 + 断行 + 分页 + 霍尔深睡（2026-09-24 收官；4 键已挪 P6） | TXT 中英混排；合盖深睡开盖 2s 恢复 —— 全过 |
| P4 ✅ | miniz + tinyxml2（锁版本，src/VENDOR.md）+ 书库 + 进度 + **EPUB 插图** | EPUB 读完一章不崩不 OOM（>280KB 章节流式）✅、章间翻页/切书/进度回页 ✅、插图清晰不重启 ✅ —— 屏上实测通过（2026-09-28；插图首测揪出坑 21 两 bug 已修复测） |
| P5 | 跨页语义：左 N / 右 N+1 | 翻 50 页内容连续 |
| P6 | 3D 打印底板 + 铰链 + 10 芯排线 + 按键小板 + **4 键逻辑**（自 P3 挪入：依赖按键小板/PCB） | 连续翻页无复位；**验证电池包深睡不自动关机** |
| P7 | 打磨：电量/封面页/VCOM 逐屏标定/温度补偿 | 待机 ≥1 月 |

> **P3 收官 ✅（2026-09-24，全项屏上目检通过）**：
> - **SD**：分目录 `/fonts` `/books`（根目录旧布局兜底）；冷上电挂载
>   **指数退避重试 ×5 + "/" 根目录探针**防"begin 成功但开文件失败"的半成功
>   挂载（坑 15 定案：卡与双屏共用 3V3、上电爬升慢，固定 100/200ms ×3 覆盖
>   不住最坏就绪窗口，连挂 9 次全败实测；根目录探针顺带修掉"冷开候选 1 静默
>   换书"——开机读英文、唤醒读中文的根因）
> - **排版**：UTF-8 中英混排分页——汉字按字断行、英文按词断、避头尾、
>   纯英文不回退、EOF 回卷；Unifont 整载 PSRAM（板型必须选 OPI，`[mem]` 常驻
>   探针可观测）
> - **字体自定义**：三档字号 16/24/32（串口 `s`）+ 四家族 EPF1 离线字库
>   classic/serif/sans/kai（`tools/mk_fontpack.py` 离线光栅化 → SD 扫描自动
>   发现 → PSRAM/heap/桶缓存三级装载 + Unifont 回退链 → NVS 持久化，
>   串口 `f` 切换即预览）
> - **霍尔深睡全链路**：合盖 300ms 防抖 → 屏上对页位置存 RTC 内存 → SPI 8 脚
>   `gpio_hold`（防深睡浮空噪声刷屏）→ EXT1 深睡；开盖唤醒重画对页进 RAM、
>   **只同步 shown 不推送面板**（VCI 常供图像没丢）= 零闪烁恢复原页，下一翻页
>   diff/DTM1 正确；loop 5s 整段 delay 改 20ms 分片轮询。台架
>   （`hall_sleep_test/`）验证 EXT1 唤醒 526–576ms、醒后仍合盖自动再睡；
>   集成后合盖睡/开盖回页/醒后翻页不花屏/多轮循环/RST 释放 hold 全过
> - **交接**：4 键过渡期用串口 `f`/`s`（`setTier`/`setFamily` 接口已备好），
>   4 键 / 电池包不自动关机验证在 P6
>
> 下一步 → **P5**：跨页语义。
>
> **P4 ✅ 收官（2026-09-24 编译 + 主机端管线验证 → 2026-09-28 屏上实测通过）**：
> - **vendored 锁版本**：miniz 3.0.2（zip 解压，原样）+ tinyxml2 10.0.0
>   （打 1 类补丁：定义 `TIXML2_PSRAM_POOL` 时 MemPoolT 4KB 池块 +
>   `_charBuffer` 文档缓冲走 ps_malloc，失败回退 malloc——>280KB 章节的 DOM
>   不挤 250KB DRAM；补丁位置/升级注意见 src/VENDOR.md）
> - **一章一转换**（`EpubBook` 组合 `TxtBook`，排版引擎零重写）：
>   miniz 流式解压章 XHTML → 片内 LittleFS 临时文件（坑 6 定案路径；**坑 18
>   起临时文件不写 SD**，SD 写会物理锁卡）→ 整章读进 PSRAM
>   （444KB 实测装得下）→ tinyxml2 解析 → DOM 抽正文（跳过 style/script/head，
>   块级补 \n，空白折叠，连续空行封顶 2）→ 2KB 缓冲写 LittleFS 临时文本 → TxtBook
>   当 TXT 翻（三档字号/四家族/避头尾全继承）。解析失败自动降级"剥标签"兜底
> - **章末推进**：TxtBook 加 setLoop(false)/hitEof()；右页中道结束画空白页，
>   下一对页自动进下一章，坏章自动跳过（guard 防死循环），全书读完回卷
> - **实体预替换**：tinyxml2 只认 5 内置实体、其余原样透传 → 解析前压缩式
>   单遍替换 &nbsp; &ldquo; &mdash; &hellip; 等（UTF-8 BOM 顺手剥掉）
> - **书库 + 进度**：开机扫 /books/*.txt + *.epub（根目录旧布局兜底），串口
>   `b` 切下一本；进度（路径+章号+章内偏移+页号）存 NVS，每 10 对页/切书/
>   深睡时落盘，冷启动按存档恢复；深睡 RTC 增加章号，开盖回页对 EPUB 同样零闪烁
> - **顺手修**：CjkFont::setFamily 同家族同档短路（否则 EPUB 每章切换会把
>   kai 几 MB 字库包从 SD 反复整载）；TxtBook::open 允许重开（关旧文件）
> - **测试书**：`tools/mk_test_epub.py` 生成 assets/books/test.epub（3 章，
>   第二章 444KB >280KB 验收线；混入中文/英文/实体/弯引号/br/嵌套/脚本）；
>   主机端用同款代码 + vendored tinyxml2 验证：三章全解析成功、实体全替换、
>   script/style 零泄漏、无三连空行

**P4 插图 ✅ 收官（2026-09-28，主机端路径解析验证 + 屏上首测 → 揪出两个
bug 已修（坑 21）→ 复烧回归通过）**：
- **管线**：转换期 `collectText` 遇到 `<img>`/`<image>`(svg 封面) 不写文本，
  写 `[[img:zip内路径]]` 标记行（src 相对本章 XHTML 解析：`../` 折叠 + `#` 截
  锚 + %XX 解码 + 空格转 %20 防拆词）→ TxtBook 排版到标记 = 独占一块行高：
  回调量高（只读文件头）→ 页中放不下整图推下页（新 s_rewindPos 页尾回退，
  与行回退互补）→ 页首硬压进一页防死循环 → EpubBook 解压图片条目到
  LittleFS 临时文件（同路径缓存，防 restartPair 重复解压）→ `EpubImg` 解码
- **解码**（详见 src/VENDOR.md）：JPEG = vendored **ChaN TJpgDec R0.03**
  （块回调无整图缓冲，RGB565 输出，3.5KB 工作区；坑 19：S3 ROM 版 esp_rom_tjpgd
  调试不可达已弃用——根因是 ChaN 输入回调"NULL buf = 跳字节"约定未实现）；
  PNG = PNGdec 库（35KB 对象堆分配，栈放不下，坑 16 同因）。
  两路都走 4×4 Bayer 有序抖动 → Paint_SetPixel 落帧缓冲（白底只点黑）；
  渐进 JPEG / 隔行 PNG / GIF / WebP 跳过不阻塞排版
- **主机端验证**：路径折叠镜像脚本对三本书全量过——test 3/3（含 ../ 引用）、
  alice_rackham 42/42、christmas_carol 11/11（含 svg xlink:href 封面）全部
  命中 zip 条目；三书图片路径最长 44B（标记 ≤52B，词缓冲 192B 无溢出）；
  全部 JPEG 均 baseline、PNG 均非隔行（解码器可解）。test.epub 的假 8B pic.png
  换成真 400×240 灰阶渐变 PNG（考缩放 + 抖动）
> - **验收清单 ✅（2026-09-28 屏上实测全过）**：①开test.epub 翻完 444KB 章不崩不 OOM
>   （串口盯 `[epub] chapter ... psram free=` 与 `[mem]`）②章间翻页连续无重页
>   ③合盖开盖回页（含章号）④`b` 切书 ⑤字体/字号切换即预览对 EPUB 生效
>   ⑥插图（alice_rackham）灰阶抖动观感可接受、无涂抹、翻到图页不重启
>   （坑 21 修复后复烧）—— 插图显示效果定案，后续不再动解码/抖动参数

## 5. 已知的坑（详见方案文档）
1. 电池包自动关机 → 深睡报废（选型硬指标）
2. 双屏同时刷新电流尖峰 → LDO 重启（大电容去耦优先，AMS1117 兜底）
3. 双屏 SPI 争用 → 严格"选片→传输→释放"，提前 return 前先放 CS
4. 绝不在 MCU 上排整本书（The Open Book：20 分钟且会崩）→ 按章排版 + SD 缓存
5. CJK：UTF-8 在分词处解码；CJK 按字符断行 + 避头尾；Unifont 存 SD 流式查表
   **→ 已落地（2026-09-24）**：字库 = GNU Unifont 18.0.01 → `tools/mk_unifont.py`
   生成平坦 `unifont.bin`（65536×32B = 2MB，`offset=cp*32` 免索引直接 seek；
   8×16 半宽左对齐补零到 16 列；辅助平面 66225 条 BMP 之外跳过；缺字画 16×16
   空心方框区分"缺字/空白"）。`CjkFont` 优先整块 ps_malloc 进 PSRAM，失败回退
   逐字形 SD seek+read。排版按像素宽：ASCII 11px / CJK 16px / 空格 11px，
   **竖版页 480 宽**（方向见 §3.7）：软预算 448px，硬上限 464px（`PAGE_W-16`，
   `Paint_SetPixel` 边界是 `>` 不是 `≥`，逻辑 x=480 会写穿显存 → 悬挂吞右边距
   也绝不能超 464）。断行四层：
   避头悬挂 → 上一 token 推挤到下一行同行（携带的前导空格剥离并恢复原
   pendingSpace）→ 普通换行 → 超长词 cutPoint 硬切。西文弯引号/长横线仍归一
   ASCII 走 Font16，英文行宽与旧版一致（不回退）。`Font12CN/Font24CN` 只有
   extern 无定义，链接不过 —— 不可用。
   **三档字号（2026-09-24 增）**：`TxtBook::setTier(16/24/32)`，默认档改
   `TxtBook.h` 的 `TXT_SIZE_PX` 再编译（P6 4 键落地后可运行期切）。S=Font16 原生
   11px+Unifont16/行高18/33行；M=Font24 原生 17px+Unifont 1.5× 最近邻/28/21行
   （笔画略参差，可接受）；L=Font16 点阵 2× 放大 22px+Unifont 2×/36/16行（整齐）。
   行数不再写死，`updateGeometry()` 按 `TXT_BOT(=PAGE_H-18)` 与墨迹盒算（换页高
   只动 `TXT_BOT`）。软行宽 = 464−汉字档宽（448/440/432），悬挂后仍 ≤464 不越屏；
   缺字方框随档 1px/2px 加粗。页眉固定 16px 不随档位，长文件名在 `drawHeader()`
   按 480 宽截断。
   **字体自定义（2026-09-24 增，Kindle 式）**：家族包 `font-<名>-<档>.bin`（EPF1）
   离线从任意 TTF 光栅化（`tools/mk_fontpack.py`），拉丁带真实 advance（比例宽度），
   汉字/全角等宽；一笔紧凑 ink（cp/adv/xy 偏移/w/h/bits），PSRAM 整包 → heap →
   SD 桶缓存三级装载，查表走 u32[257] 桶索引线性扫（≤256 条）。缺字回退：
   家族包 → Unifont（对齐包基线）→ 方框。行几何随包度量联动（基线/行盒/行距
   = 盒+档宽/8）；软预算与 classic 同为 464−档宽（悬挂另有显式 HARD 检查兜底）。
   切换：串口 `f`=换家族 `s`=换字号（P6 4 键落地后绑同一接口），选择存 NVS 断电保留；
   `restartPair()` 回跳当前对页立即重排 = 切换即预览。家族 classic/serif/sans/kai
   四款全就绪（kai=霞鹜文楷 43433 字形，2026-09-24）。 ink 出界自兜底（竖版页逻辑 x≤479/y≤647，Paint 边界是 > 不是 ≥）。
6. EPUB >280KB 章节崩分配器（atomic14 #73）→ 先解压到临时文件再流式（坑 18 后 = 片内 LittleFS）
7. SDMMC × core 2.0.17 × S3 兼容性 → **P3 首验（2026-09-21）：4-bit 不稳定，定案 1-bit @20MHz；写 2MB / 1417ms ≈ 1.41MB/s**。**二验（2026-09-24）：正式引脚 38/39/40 挂载成功，/test.txt(151KB) 流式分页正常（24 行/页，读+渲染 ~30ms/页，对比 1.3s 刷屏可忽略）**。
   ⚠️ 踩坑：初始化 `0x107`（卡无应答）→ **CMD/D0 需要内部上拉兜底**（`pinMode(INPUT_PULLUP)` 再 `setPins`）——模块没焊总线上拉时 CMD 浮空；8.21 的 one_bit_write 带这两行、初版 Storage 漏抄。D1/D2/D3 悬空无妨（1-bit 用不到）。
   **写后读回校验 → P4 首测未过（2026-09-28，坑 18）**：正式引脚写 ~3KB 即 0x109
   锁卡；EPUB 已改片内 LittleFS 临时文件绕开 SD 写。
   待补：读速专项实测、深睡唤醒后重新挂载；写路径硬件复测用
   `sdcard/one_bit_write`（已改 38/39/40）；真不稳再回退 SPI（接口不变）
8. 同型号两块屏 VCOM 不一致 → CDI(0x50) 左右分开标定（Diptyx 做法）
9. 残影 → 每 5 次快刷插一次全刷
10. `0xE5` 强制温度 → 对比度略降；后续用面板温度传感器（0x40 TSC/0x43 TSR）补偿
11. 结构装配时 USB 座外壳通孔必须焊/固定，否则连焊盘撕掉（Open Book 教训）
12. GUI_Paint 用前必须 `Paint_NewImage(buf, W, H, PAGE_ROTATE, WHITE)`（W/H 是
    **显存** 648×480，旋转定竖版页方向，见 §3.7），之后各画布只
    `Paint_SelectImage` 切换 —— 漏了 NewImage 不报错不崩溃，只是全部绘制静默丢弃
    （P1 花屏根因）。宽高/旋转只认 NewImage 那一次，SelectImage 只换指针
13. 右屏 BUSY 用 GPIO48 = DevKitC-1 的 RGB LED 脚：LED DIN 高阻无电气冲突，
    但刷新时 BUSY 信号会让 LED 闪 —— 属正常现象，别当故障查接线
14. 设计外壳时预留出esp32s3的rst和boot按钮针孔
15. SD 冷上电首挂偶发 0x107（卡与双屏共用 3V3 轨，上电爬升慢、卡 POR 就绪晚；
    复位键不断卡的电，故 reset 后首挂即成。2026-09-24 实测：固定 100/200ms
    重试 ×3 覆盖不住最坏就绪窗口，4 个候选文件各试 3 次连挂 9 次仍全败）→
    `Storage::begin()` 改**指数退避 ×5**（200/400/800/1600ms，每次重试重拉
    CMD/D0 上拉 + setPins，end() 会拆引脚配置），挂载成功后加**根目录探针**
    （open("/") 失败视为半挂载拆了重来——挂载过了但首开文件失败的症状同源，
    TxtBook/CjkFont 开文件另带 100ms 退避重试 ×3）；挂载失败不缓存只是兜底，别依赖它
16. miniz `extract_to_callback`/`extract_to_mem*` 把 `tinfl_decompressor`（≈8.2KB）
    放**栈**上 → `loopTask` 默认 8KB 栈进函数即溢出（2026-09-24：开机串口 `b` 切到
    EPUB 直接 reboot，栈顶 `|<-CORRUPTED`）→ `EpubZip` 改走迭代式
    `extract_iter_new/read/free`（解压器状态在堆分配的 iter_state 里），零补丁，
    详见 `src/VENDOR.md`。**排查口诀**：EPUB 独有路径崩 = 先查 miniz/tinyxml2 的
    栈上大数组，TXT 路径不经过它们所以"之前一直没事"
17. miniz 32 位堆越界（上游 richgel999/miniz#371，**未发布修复**）：`tinfl_decompress`
    在 xtensa 32 位上写越 `tinfl_decompressor` 尾部（64 位主机不复现），落在
    `mz_zip_reader_extract_iter_state` 尾部字段上 → `file_crc32` 被踩成确定性
    字节序列，`iter_free` 的 CRC 校验必败。两段缓解（`EpubZip.cpp` 头注释）：
    ① 所有 miniz 分配 +4096 超配（保护堆上相邻分配）② 调用侧用栈上 `own_crc`
    回填 `file_crc32`（栈变量越界够不着）——尺寸检查保留。诊断法见代码内注释
18. **SD 卡写入物理锁卡**（P4 首测 2026-09-28）：读全程正常（TXT 分页、zip 流读、
    字库 seek 都稳），但正式引脚 38/39/40 写临时文件 ~3KB 后驱动层报 0x109
    （应答 CRC）→ 此后**读写全 0x107 超时**，`Storage::recover()`（CMD0 重挂）
    也救不回，只能断电重插 = 卡写入期 VDD 塌陷/信号边际的典型症状（小文件
    244/821B 写得过，持续写电流尖峰才顶不住；也不是 miniz 踩内存——全程
    `heap ok`）。注意：2MB 写测速是**旧引脚 10/11/12**（现屏脚）的成绩，
    正式引脚从来没验过写（§5.7 待补项）。→ 固件侧已根治：EPUB 临时文件
    （container/OPF/xhtml/文本）全部改走**片内 LittleFS**（默认 16M 分区表的
    spiffs 分区 3.5MB，`LittleFS.begin(true)` 首启自动格式化），全项目对 SD
    零写入（进度在 NVS），EPUB 不再受卡写能力影响。硬件侧待办（做板 checklist
    见 WIRING §4）：卡座 VDD 10µF+100nF 就近去耦、CMD/D0 实体 47k 上拉、
    换卡排除、仍不稳则降频 10MHz；复测工具 `sdcard/one_bit_write`（已改 38/39/40）
19. **串口 RX 幽灵命令 + drain 吞输入回归（2026-09-28）**：三个阶段——
    ① 原始症状：EPUB 深睡唤醒后没发过 `b`，书被切走（日志反推 loop 首轮收到
    'b' 又收到 'h'，均紧跟 `full refresh`）→ 疑似全刷串扰 RX0（GPIO44，UART0，
    CDCOnBoot=Disabled）。
    ② 加 `drainSerial()` 防护后引出**回归**：drain 在 `runRefresh` 尾执行，而
    `handleSerial` 只在 loop 顶部跑一次 → **等待期（5s，用户输入主窗口）收到的
    字节滞留到刷新结束被 drain 全吞**，实测 7 个 `b\n`（`62 0a`）全灭、`b` 切书
    100% 失效。教训：**丢字节的防护必须放在字节到达的窗口内，不能放在下游**。
    ③ 终态方案（三窗口各归各位）：等待期改**每 20ms 轮询** `handleSerial` 即时
    执行（永不丢）；刷新期/开机期用 `harvestRx()` **收割验证**——恰好"一个命令
    字符 + 换行"（终端实测发 `62 0a`）入队补执行，裸字节/乱码打 hex 记
    `[rx] dropped`，纯换行（用户命令尾字节滞留）记 `[rx] ignore` 单独归档。
    验证：`b` 即时执行、三次深睡恢复全部准确续页、深睡后无幽灵切书。
    硬件侧仍待观察：RX 走线远离 SPI/屏升压、共地，接 USB-UART 小板时检查杜邦线
    是否和屏线绞在一起；若再出现 `[rx] dropped` 含命令字符的随机字节即噪声实锤。
20. **深睡自醒（待定案 2026-09-28）**：首次深睡后无故自醒（用户未开盖），后两次
    正常开盖醒。日志事实：那次也是 `cause=3 (EXT1)`，即 **GPIO4 真实跳到开盖电平
    才触发的唤醒**；且醒来后霍尔持续读"开盖"跨了两个翻页周期（约 19s）才重新
    "合盖"入睡——瞬态串扰毛刺做不到这点，只剩两种解释：(a) 盖子/磁铁实际被碰到
    （或合盖时磁铁就处于 DRV5032 阈值边际，静置后漂移出窗口）；(b) 传感器供电/
    走线在深睡初期有慢恢复扰动。同轮唤醒还观察到 SD 首挂 0x107 重试 1-2 次成功
    （坑 15 的唤醒版）。固件侧已加三路取证（下轮日志定案）：
    ① `[wake] slept Ns` 入睡→唤醒实际时长（RTC 时钟跨深睡，`s_sleepAt`）——
    **秒级 = 入睡瞬间毛刺/电平边际；分钟级 = 盖子真被打开过**
    ② `[wake] ext1 pins=0x..` 触发脚位掩码（确认是 GPIO4 不是别的脚）
    ③ `[wake] hall=N (closed/open)` pinMode 后立刻读——**EXT1 醒 + hall=closed
    = 毛刺自醒实锤**（醒来时电平已回落）；EXT1 醒 + hall=open = 盖子确实开着
    硬件侧排查（定案后再动手）：磁铁与 DRV5032 对位/气隙、合盖后轻敲看电平是否
    跳、GPIO4 走线远离屏升压。
21. **EPUB 插图"糊成一片" + 翻到特定页重启（P4 屏上首测 2026-09-28，主机端复现
    定案）**：两个独立 bug，alice_rackham.epub 上一个表现为显示糊、一个表现为翻到
    title.png 那页堆崩：
    ① **JPEG 对角涂抹（糊的根因）**：tjpgd 的 `JRECT` 是**闭区间**，`workbuf` 里
    `rx×ry` 个像素紧凑排列、步进 = `right-left+1`；`EpubImg::jpgOut` 少写 `+1`
    → 每行读取起点左移一格、逐行累积成 45° 斜纹，16×16 MCU 块还丢末行末列。
    主机端用同款 vendored tjpgd 解 cover.jpg 对比新旧索引：**83.9% 像素不同**
    （旧=斜纹糊成一片 / 新=清晰封面，`/tmp` 下 jpgtest）。顺带修：`JDEC jd` 未
    初始化，而 Bodmer 补丁版 `jd_prepare` **保留**入参 `jd.swap`（先读后清零回填）
    → 栈垃圾非 0 时 RGB565 整体字节交换、lum565 亮度全错；现显式 `memset`。
    ② **PNGdec 奇数宽度越界写（崩的根因）**：`PNGRGB565` 调色板路径按 `x+=2`
    （4bpp）/`x+=4`（2bpp）整组步进，奇数宽多写 1~3 个像素（上游库缺陷，未修）。
    title.png = 373 宽（奇数）→ 恰好 2 字节写穿 `malloc(373*2)` 的尾部魔数，
    下次 `free` 报 `CORRUPT HEAP: Bad tail ... 0xbaad5678 got 0xbaad0000` +
    `multi_heap_free` 断言重启——日志里崩溃点正是 title.png 解码途中（页 4 有
    提取记录却无 `[epub] image ... ok`）。主机端金丝雀实测：373 宽越 2 字节、
    372/400 宽 0 字节，与设备日志逐字节吻合。缓解（库未改）：`pngDrawCb` 行缓冲
    多留 4 像素吸收越界写，多出的垃圾像素不进画布。
    **排查口诀**：屏上图"糊"先怀疑采样/索引（步进、坐标映射），别急着怪分辨率；
    `CORRUPT HEAP ... Bad tail` = 精确越界 2~8 字节，先查行缓冲按宽度分配的地方。

## 6. 本地工具
- 编译：`~/.local/bin/arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default /home/roba/Arduino/eink`
- 竖版页离屏回归（不接硬件，§3.7）：`GUI=$HOME/Arduino/libraries/ESP32epdx/src/GUI g++ -std=c++17 -I"$GUI" tools/rot_layout_test.cpp "$GUI/GUI_Paint.cpp" "$GUI/font16.cpp" -o /tmp/rot_layout_test && /tmp/rot_layout_test`
  （断言逻辑画布 480×648、最坏排版零越界、页顶朝右；改动排版常量后跑一下）
- 字库生成：`python3 tools/mk_unifont.py`（源 `unifont_all.hex` → `assets/fonts/unifont.bin`，已生成）
- **字库包（字体自定义）**：`python3 tools/mk_fontpack.py <font.ttf> assets/fonts --name <名字>
  [--sizes 16 24 32]` → `assets/fonts/font-<名字>-<档>.bin`（EPF1 格式：紧凑 ink 记录 +
  真实步进 + 全局基线度量，依赖 Pillow/fontTools）。已生成：serif（Noto Serif SC）、
  sans（Noto Sans SC）、kai（霞鹜文楷 LXGW WenKai，OFL 开源，三款都可自由转换）
  （kai 三档 43433 字形已生成，2026-09-24）
  - 设备启动扫 `/fonts/font-*.bin` 自动发现家族；**加新字体 = 跑一条命令 + 拷文件**
  - 行盒度量封顶 size×1.25/size×0.4（防竖排假名等离谱 ink 撑爆行距）
  - 阈值 96 二值化（Noto 细横笔灰度仅 ~108，128 会砍掉）
- **SD 卡目录布局**（与 `assets/` 一一对应，`cp -r assets/. <SD卡>/` 即部署完）：
  - `/fonts/`：`unifont.bin`（2MB，classic 模式 + 缺字回退，必需）+ 可选
    `font-<名字>-<16|24|32>.bin`（字体家族，缺某档自动回 classic）
  - `/books/`：电子书 `*.txt` **和 `*.epub`**（P4 起书库扫描两类，串口 `b` 切换）
  → 缺 `unifont.bin` 时汉字全画 16×16 空心框
- 霍尔深睡台架测试：`~/.local/bin/arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=default /home/roba/Arduino/eink/hall_sleep_test`
  （合盖 300ms 深睡 / EXT1 开盖唤醒 / RGB 48 可视判定；极性反了改 `HALL_CLOSED_LEVEL`）
- 资料：`~/Downloads/Gdey0583T81/`（屏规格书 + UC8179 数据手册 + 官方 demo 压缩包）

## 7. 双核并行备忘（2026-09-24 讨论 · 未采用，P4 按数据再评估）

**问题**：要不要 core0 读 SD、core1 渲染？

**结论：现阶段不值得** —— 瓶颈是电子纸物理刷新，不是计算。重叠掉的渲染只占周期 4–5%：

| 阶段 | 实测（fast 页周期） | 占比 |
|---|---|---|
| SD 读 + 解码 + 排版渲染 ×2 页 | 50–73ms（25–37ms/页） | ~4–5% |
| SPI 搬帧（27.6KB×2 @10MHz） | ~16ms | ~1% |
| 物理刷新 BUSY 等待 | **1311ms**（全刷 3826ms） | ~94% |

SD 读本身每页仅 1–2ms（1-bit@20MHz ≈ 2.5MB/s，每页几 KB）。

**讨论里站得住的两个出处**（不是速度，是这两点）：
1. 全刷 3.8s 期间 `present()` 阻塞 → 串口 `f`/`s`、霍尔轮询全停 = 固件"假死感"
2. P4 EPUB 章节头一次解析（miniz 解 280KB + XML 扫描）可能几百 ms，
   落在章节首页 = 可感知卡顿

**若将来并行，切口别切在"SD vs 渲染"**：`TxtBook` 是紧耦合流式管线
（读 chunk → UTF-8 解码 → 断词断行 → 直接画 fb），中间架生产者-消费者队列
复杂度高、收益 ≈0（读只占 1–2ms）。正确切口 = **整页预取**：后台任务独占
`txt` + SD + Paint，把下一对页渲进备用缓冲（三缓冲轮换 6 个 fb ≈ 166KB，
PSRAM 无压力），前台 loop 只 present + 换挡，换挡点一道栅栏。单写者模型天然无锁。

**本代码库会咬人的点**：
- `GUI_Paint` 是**全局单例**（`Paint_NewImage` 一个全局结构）：渲染必须始终
  单核单写者；想左右页各一核并行画 = 必花屏（P1 花屏同族问题）
- `TxtBook` 游标状态（`m_file/m_page/m_pairStart`）：`f`/`s` 的
  `restartPair()+renderPair()` 必须改成给渲染任务发消息，主线程不许直接碰
- 深睡协调：SPI 在飞绝不能睡（图像断半截）；霍尔触发要等 present 完成 +
  渲染任务静默，才能 `gpio_hold` → 单线程纪律升级成双任务握手
- 可行性无问题：`loopTask` 默认就在 core1，WiFi/BT 关着，core0 基本闲着

**更便宜的中间方案**（同样收益、零竞态）：`present()` 拆成"发帧 → 等 BUSY"
两段，在 BUSY 等待的 1300ms 里**单线程**渲染下一对页 —— 串口/霍尔在刷新间隙
即响应，省下的毫秒数与双核方案几乎相同。

**决策路径**：
1. 现在：不动（单线程 1.36s/页，达标 ≤1.8s）
2. P4 实测单页渲染 >100–200ms 或章节首页卡顿明显 → 先上"BUSY 期间渲染下一页"
3. 仍不够 → 再上 core0 预取任务（上面的单写者设计），收益门槛到了再付复杂度
