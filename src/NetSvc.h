#ifndef NET_SVC_H
#define NET_SVC_H

#include <Arduino.h>
#include <FS.h>

// 系统网络服务（系统菜单的后端，DESIGN §8）：
//   WiFi 配置 = SoftAP 配网页（热点 eink-book-xxxx → http://192.168.4.1/wifi
//               网页里扫描/选 SSID 输密码，存 NVS 后重启连接）
//   WiFi 开关 = 射频总开关（NVS 持久化，**默认关** = 设计 §2 "WiFi 默认关" 省电）
//   OTA 开关   = ArduinoOTA（arduino-cli 网络上传）+ 网页 /update 传 .bin，同一开关
//   上传服务   = 网页 /upload 上传 *.txt/*.epub → SD /books/，写完回调 onBookAdded；
//                /library 书库管理（列表/下载/删除），删完回调 onBookRemoved。
//                两个页面共用"电子书上传服务"开关（= 网页书库服务总开关）。
//
// 单 WebServer(:80) 承载全部页面，随 WiFi 开关整体起停；所有方法只在 loop 等待窗
// 被轮询（NetSvc::loop），不碰屏 SPI —— 上传/OTA 的闪存写都落在翻页间隙，
// 不会打断 e-ink 刷新。/wifi 的扫描会阻塞 1-3s，同样落在等待窗（翻页顺延）。
//
// 故障策略：STA 15s 连不上（密码错/路由器不在）→ 自动回落开配置热点，
// 屏幕菜单里永远救得回来。深睡前调 beforeSleep() 断电 WiFi。
class NetSvc {
public:
  enum WifiState : uint8_t { OFF, PORTAL, CONNECTING, CONNECTED };

  void begin();         // 读 NVS + 装 HTTP 路由；wifi 开且有凭据 → STA 连接
  void loop();          // handleClient + OTA handle + STA 超时回落 + 延迟重启
  void beforeSleep();   // 深睡前：停服务、关 WiFi（合盖深睡不带电）

  // —— 菜单动作（全部立即生效并写 NVS）——
  void setWifiOn(bool on);
  void setOtaOn(bool on);
  void setUpOn(bool on);
  void startPortal();   // "WiFi 配置"：开热点配网页（wifi 开关顺带置开）

  bool wifiOn() const { return m_wifiOn; }
  bool otaOn() const { return m_otaOn; }
  bool upOn() const { return m_upOn; }
  bool portalUp() const { return m_portal; }
  WifiState state() const;
  const char* staSsid() const { return m_ssid; }
  const char* apSsid() const { return m_apSsid; }
  const char* ip();          // 当前可达 IP 文本（STA 优先，否则热点；覆写内部缓冲）
  String stateText() const;  // "关闭/热点 xxx/连接 xxx…/已连接 xxx"（中文，屏+网页共用）

  // 上传完成回调：path 已写入 SD /books（.ino 里重扫书库/重开当前书）
  void (*onBookAdded)(const char* path) = nullptr;
  // 网页删除回调：path 已从 SD 删除（.ino 里清书签/进度、重扫、必要时换书）
  void (*onBookRemoved)(const char* path) = nullptr;

  // ---- /debug 网页调试台（DESIGN §11）：设备侧状态注入 + 动作回调 ----
  // 屏幕全挂时的救砖通道：状态经 onDbgInfo 从 eink.ino 注入；动作回调在 loop
  // 等待窗上下文执行（与 onBookAdded 同上下文，可以安全刷屏）。WiFi 开关是这页
  // 的门槛（深睡关 WiFi 时网页本就不可达，属 §2 设计）
  struct DbgInfo {
    const char* mode;       // 显示模式："双屏"/"仅左屏"/"仅右屏"/"无头"
    bool leftOk, rightOk;   // 面板存活（开机 begin 结果 + 运行期超时降级）
    int busyL, busyR;       // BUSY 引脚电平（HIGH = 空闲）
    const char* hallText;   // 霍尔："开盖"/"合盖"（极性由 eink.ino 解释）
    uint32_t pageCount;     // 累计刷新次数
    uint32_t lastMs;        // 上次刷新耗时
    bool lastOk;            // 上次刷新是否 BUSY 超时
    const char* lastTag;    // 上次刷新来源（fast/menu/home…）
    uint32_t fastN, fastAvg, fastMin, fastMax, fastTo;   // PageStats 汇总
    uint32_t fullN, fullAvg, fullMin, fullMax, fullTo;
    int libCount;           // 书库本数
    bool bookOk;            // 有在读的书
    const char* bookName;   // 当前书名（"-" = 无书）
    long page;              // 当前对页号（-1 = 无书）
    int pct;                // 进度%（-1 = 未知）
    bool sdOk;              // SD 挂载成功
    unsigned long sdUsedMb, sdTotalMb;
    uint32_t keysBounce, keysOvf;   // 按键去抖丢弃 / 队列溢出（诊断）
  };
  void (*onDbgInfo)(DbgInfo&) = nullptr;   // GET /debug 时填状态
  void (*onDbgPanel)(bool left) = nullptr; // 复位某屏（硬复位 + 重新 init）
  void (*onDbgRefresh)() = nullptr;        // 强制重画当前屏（按 UI 上下文）
  void (*onDbgFlip)(int delta) = nullptr;  // 翻页测试（±1 对页）

private:
  void setupHttp();
  void ensureServer();
  void startSta();
  void startOta();
  void handleIndex();
  void handleWifiPage();
  void handleWifiSave();
  void handleUploadForm();
  void handleUploadResult();
  void uploadChunk();
  bool upWrite(const uint8_t* p, size_t n);   // 512B 对齐分块写，false=写不动
  bool upFlush();                             // 写掉尾部不足 512B 的余量
  void upAbort(const char* msg);              // 关句柄→重挂→删半截文件（msg 可为空）
  void handleUpdateForm();
  void handleUpdateResult();
  void updateChunk();
  void handleLibrary();                       // /library：书库列表/下载/删除
  void handleLibraryDel();
  void handleLibraryGet();
  void handleDebug();                         // /debug：调试台（状态+动作+日志）
  void handleDebugLog();                      // /debug/log：日志环增量（text/plain）
  void handleDebugAct();                      // /debug/act：复位面板/刷新/翻页/重启

  bool m_wifiOn = false;   // NVS 默认 false（省电）
  bool m_otaOn = true;     // NVS 默认 true：WiFi 一开即可 OTA（开关语义直观）
  bool m_upOn = true;      // 同上
  bool m_portal = false;   // 配置热点在开
  bool m_serverUp = false;
  bool m_otaUp = false;    // ArduinoOTA.begin() 过
  bool m_staTrying = false;
  uint32_t m_staTryAt = 0;
  uint32_t m_restartAt = 0;   // >0 = 到点延迟重启（wifi 保存后让响应先发完）
  char m_ssid[33] = "";
  char m_pass[65] = "";
  char m_apSsid[20] = "eink-book";
  char m_ip[20] = "";

  // 上传会话（WebServer 分块回调驱动，单会话）
  File m_upFile;
  char m_upPath[96] = "";
  const char* m_upMsg = nullptr;   // 失败原因（全部指向静态字面量）
  bool m_upActive = false;
  bool m_upDone = false;
  size_t m_upBytes = 0;       // 已落盘字节数（END 时和 u.totalSize 对账）
  uint8_t m_upBuf[512] = {0}; // 512B 累加器：只整扇区写 → 每次 f_write 都是单块 CMD24
  size_t m_upFill = 0;
  bool m_updBlocked = false;       // OTA 关时收到 /update 上传 → 拒绝且不碰 Update
};

extern NetSvc net;

#endif  // NET_SVC_H
