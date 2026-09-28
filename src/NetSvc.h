#ifndef NET_SVC_H
#define NET_SVC_H

#include <Arduino.h>
#include <FS.h>

// 系统网络服务（系统菜单的后端，DESIGN §8）：
//   WiFi 配置 = SoftAP 配网页（热点 eink-book-xxxx → http://192.168.4.1/wifi
//               网页里扫描/选 SSID 输密码，存 NVS 后重启连接）
//   WiFi 开关 = 射频总开关（NVS 持久化，**默认关** = 设计 §2 "WiFi 默认关" 省电）
//   OTA 开关   = ArduinoOTA（arduino-cli 网络上传）+ 网页 /update 传 .bin，同一开关
//   上传服务   = 网页 /upload 上传 *.txt/*.epub → SD /books/，写完回调 onBookAdded
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
  void handleUpdateForm();
  void handleUpdateResult();
  void updateChunk();

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
  size_t m_upBytes = 0;
  bool m_updBlocked = false;       // OTA 关时收到 /update 上传 → 拒绝且不碰 Update
};

extern NetSvc net;

#endif  // NET_SVC_H
