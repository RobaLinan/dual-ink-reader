# 接线表（PCB 制图用）· 双屏电子书 v4

> 依据：DESIGN.md §1.1、`eink.ino` 引脚宏、`src/Storage.cpp` SDMMC 定案（2026-09-24 核对）。
> 主控 ESP32-S3 N16R8；**全部信号 3.3V 电平**；屏 SPI = 10 MHz / MODE0 / MSBFIRST，SDMMC 1-bit @20 MHz。
> ⚠️ 屏/SD/霍尔已实测（霍尔深睡随 P3 收官，2026-09-24）；**按键是 P6 预分配脚尚未接线验证**——脚位如下，代码后补。

## 1. GPIO 总表

| GPIO | 网络名 | 方向 | 对端 | 备注 |
|---|---|---|---|---|
| 12 | EPD_SCK | 出 | 屏L CLK + 屏R CLK | 两屏并联（SPI2） |
| 11 | EPD_MOSI | 出 | 屏L DIN + 屏R DIN | 两屏并联 |
| 14 | EPD_L_CS | 出 | 屏L CS | 低有效，选片→传输→释放 |
| 15 | EPD_L_DC | 出 | 屏L D/C | |
| 16 | EPD_L_RST | 出 | 屏L RES | 低有效 |                       
| 17 | EPD_L_BUSY | 入 | 屏L BUSY | **高 = 空闲**（此模组极性） |
| 18 | EPD_R_CS | 出 | 屏R CS | 经铰链排线 J3 |
| 21 | EPD_R_DC | 出 | 屏R D/C | 经 J3 |
| 47 | EPD_R_RST | 出 | 屏R RES | 经 J3 |
| 48 | EPD_R_BUSY | 入 | 屏R BUSY | 经 J3；DevKitC-1 与 RGB LED 同脚，刷新时 LED 闪属正常 |
| 38 | SD_CLK | 出 | SD CLK | SDMMC 1-bit，不需要上拉 |
| 39 | SD_CMD | 双向 | SD CMD | **47kΩ↑→3V3**（SD 规范 10–100k；固件内部上拉只是兜底，0x107 教训） |
| 40 | SD_D0 | 双向 | SD DAT0 | **47kΩ↑→3V3**，同上 |
| — | SD_DAT1/2/3 | — | 不接 | 1-bit 模式用不到，悬空无妨 |
| 1 | BTN_PREV | 入 | 按键小板 J5 | 低有效，内部上拉 + 板载 10k，ext1 唤醒 |
| 2 | BTN_NEXT | 入 | 按键小板 J5 | 同上 |
| 9 | BTN_MENU | 入 | 按键小板 J5 | 同上 |
| 10 | BTN_FONT | 入 | 按键小板 J5 | 同上 |
| 4 | HALL_IN | 入 | DRV5032 OUT（J6） | 推挽输出无上拉，ext1 唤醒 |
| 8 | BAT_SENSE | 入 (ADC1_CH7) | 5V 分压（J7） | **可选**；电池包自带电量指示则删掉整段 |
| 5 / 6 / 7 / 41 / 42 / 13 | — | — | 备用 | 可作 SD_CD、状态 LED 等 |

## 2. 禁用引脚（封装上凡涉及一律 NC）

| GPIO | 原因 |
|---|---|
| 0 | strapping，仅可接 BOOT 键到 GND，勿接其它外设 |
| 3 / 45 / 46 | strapping，悬空或保持默认 |
| 19 / 20 | USB D−/D+，只连 Type-C 座，否则留空 |
| 26–32 | N16R8 **内部 Flash 占死**（P3 教训：CMD 误接 29 → 0x107 卡无应答） |
| 33–37 | 八线 OPI PSRAM 占用 |
| 43 / 44 | UART0 TX/RX，只作调试串口引出，勿作 GPIO |

## 3. 连接器明细

### 3.1 J1 左屏 / J2 右屏 —— GDEY0583T81 转接板（8 针）

芯序以转接板**丝印**为准，下表锁信号不锁物理顺序：

| 转接板信号 | 网络 | J1 左屏 | J2 右屏 |
|---|---|---|---|
| VCC (VCI) | 3V3 | 3V3，**就近 100µF + 100nF** | 3V3（经 J3 供电），同样去耦 |
| GND | GND | GND | GND |
| CLK | EPD_SCK | GPIO12 | GPIO12 |
| DIN | EPD_MOSI | GPIO11 | GPIO11 |
| CS | EPD_CS | GPIO14 | GPIO18 |
| D/C | EPD_DC | GPIO15 | GPIO21 |
| RST | EPD_RST | GPIO16 | GPIO47 |
| BUSY | EPD_BUSY | GPIO17 | GPIO48 |

- 右屏**物理旋转 180° 安装**，镜像由固件 `mirrored=true` 处理。
- 深睡时屏 VCI **保持供电**（DSLP ≈1µA），唤醒直接差分刷新——不要加受控断电开关。

### 3.2 J3 铰链 10 芯排线（左半主板 ↔ 右屏转接板）

| 芯 | 网络 | 芯 | 网络 |
|---|---|---|---|
| 1 | 3V3 | 6 | GND |
| 2 | GND | 7 | EPD_R_CS (GPIO18) |
| 3 | EPD_SCK (GPIO12) | 8 | EPD_R_DC (GPIO21) |
| 4 | GND | 9 | EPD_R_RST (GPIO47) |
| 5 | EPD_MOSI (GPIO11) | 10 | EPD_R_BUSY (GPIO48) |

- 地芯贴时钟/数据芯（串扰考虑）；如更需备用芯，把 4 或 6 的 GND 改 NC。
- 单屏刷新峰值约 200mA 经此线，3V3/GND 用满线径。

### 3.3 J4 microSD 卡座（板载）

| 卡座脚 | 网络 | 主控 | 备注 |
|---|---|---|---|
| VDD | 3V3 | — | 卡是 3.3V 器件，**严禁 5V**；座旁 10µF + 100nF |
| GND | GND | — | |
| CLK | SD_CLK | 38 | 无需上拉 |
| CMD | SD_CMD | 39 | **47kΩ↑→3V3** |
| DAT0 | SD_D0 | 40 | **47kΩ↑→3V3** |
| DAT1/2/3 | NC | — | 1-bit 不用，悬空无妨 |
| CD | 可选 | 5/6/7 | 固件暂不用；若接：低=有卡，配 100kΩ↑ |

### 3.4 J5 按键小板（6P 排线）

| 芯 | 网络 | 主控 |
|---|---|---|
| 1 | BTN_PREV | GPIO1 |
| 2 | BTN_NEXT | GPIO2 |
| 3 | BTN_MENU | GPIO9 |
| 4 | BTN_FONT | GPIO10 |
| 5 | 3V3 | 上拉电源 |
| 6 | GND | |

小板上每键：**10kΩ 上拉到 3V3 + 轻触开关到 GND**（低有效）。GPIO 1/2/9/10 是 RTC IO，可 ext1 唤醒。

### 3.5 J6 霍尔（3P，DRV5032 模块）

| 芯 | 网络 | 说明 |
|---|---|---|
| 1 | 3V3 | **常供电**（深睡电流 µA 级，勿切电） |
| 2 | GND | |
| 3 | HALL_OUT | → GPIO4；推挽输出，无上拉。买**全极性开关型**（锁存型则唤醒逻辑改"任一沿都醒"） |

若板载贴片 DRV5032：VCC–GND 间加 100nF。

### 3.6 J7 电池输入 + 电量检测（电量段可选）

| 网络 | 连接 | 备注 |
|---|---|---|
| 5V_IN | 电池包 +5V → 主控 5V/VIN | 电池包**必须无自动关机/支持常开**（选型硬指标） |
| GND | 电池包 − | |
| BAT_SENSE | 5V —R1 100kΩ— GPIO8 —R2 100kΩ— GND，节点对地 100nF | 分压比 ½，5V→2.5V；100k/100k 常耗 25µA，深睡在意就改 470k/470k（读数不变） |

### 3.7 （仅当 PCB 直接承载主控，而非插 DevKit）

- **EN**：10kΩ↑3V3 + 复位键→GND（可加 1µF 延时）。
- **BOOT**：GPIO0—按键—GND——GPIO0 唯一合法外设。
- **USB**：D−→GPIO19、D+→GPIO20；Type-C 座 CC1/CC2 各 5.1kΩ→GND；**外壳通孔必须焊**（DESIGN §5.11，否则连焊盘撕掉）。
- **3V3 电源**：5V 经 LDO。双屏刷新峰值 300–400mA + 主控余量一起估；**深睡要 µA 级就勿用 AMS1117 当主 LDO**（静态 ~5mA），AMS1117 只作"双屏尖峰重启"的回退方案（DESIGN §2）。

## 4. 制板 checklist

- [ ] 每屏 VCI：100µF + 100nF 就近转接板
- [ ] SD 座 VDD：10µF + 100nF；CMD/D0 各 47kΩ 上拉
- [ ] 4 个按键上拉在小板侧，主控侧直连
- [ ] 屏 SCK/MOSI 并联走线到 J1/J2；可预留 22Ω 串阻位（先 DNP，长排线振铃再焊）
- [ ] 深睡不断电：屏 VCI、霍尔 3V3 常供
- [ ] §2 禁用脚全部 NC；strapping 脚不拉外部电平
- [ ] 右屏 180° 安装位标注在丝印上
- [ ] （载板时）USB 座外壳通孔焊接

## 5. 代码对应

| 位置 | 内容 |
|---|---|
| `eink.ino:21-37` | 屏 + 按键 + 霍尔 + 电量引脚宏 |
| `src/Storage.cpp:12-16` | SDMMC 38/39/40 + CMD/D0 内部上拉兜底 |
| `DESIGN.md §1.1` | 引脚分配总纲（改脚需三处同步） |
