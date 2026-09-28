#include "NetSvc.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>
#include <SD_MMC.h>

#include "Storage.h"

// 单例 + 单 WebServer：路由全部在 setupHttp 一次性装好，起停只管 begin/stop
NetSvc net;
static WebServer s_srv(80);
static Preferences s_np;   // NVS 独立命名空间 "eink-net"（书进度的 "eink-book" 不混）

// ---- 小工具 ----

// 只留文件名（剥掉客户端可能带上来的路径），再滤 FAT 非法字符。返回 false = 空名
static bool sanitizeName(const char* in, char* out, size_t n) {
  if (!in || !*in) return false;
  const char* base = in;
  for (const char* p = in; *p; p++)
    if (*p == '/' || *p == '\\') base = p + 1;
  size_t j = 0;
  for (size_t i = 0; base[i] && j + 1 < n; i++) {
    unsigned char c = (unsigned char)base[i];
    if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|')
      continue;
    out[j++] = (char)c;
  }
  out[j] = '\0';
  return j > 0;
}

// 扩展名匹配（忽略大小写）；与 eink.ino 的 hasExt 同规则
static bool extIs(const char* name, const char* ext) {
  size_t nl = strlen(name), el = strlen(ext);
  if (nl <= el) return false;
  const char* s = name + nl - el;
  for (size_t i = 0; i < el; i++)
    if (tolower((unsigned char)s[i]) != tolower((unsigned char)ext[i])) return false;
  return true;
}

static void htmlEscape(String& out, const String& v) {
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
}

static const char kHead[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;margin:1.2em;max-width:28em}"
    "input,button{display:block;width:100%;margin:.6em 0;padding:.6em;font-size:1em}"
    "li{margin:.4em 0}a{margin-right:.6em}</style>";

// ---- 生命周期 ----

void NetSvc::begin() {
  s_np.begin("eink-net", false);
  m_wifiOn = s_np.getBool("wifi", false);   // 默认关（DESIGN §2 省电定案）
  m_otaOn = s_np.getBool("ota", true);
  m_upOn = s_np.getBool("up", true);
  String s = s_np.getString("ssid", "");
  snprintf(m_ssid, sizeof(m_ssid), "%s", s.c_str());
  s = s_np.getString("pass", "");
  snprintf(m_pass, sizeof(m_pass), "%s", s.c_str());

  uint64_t mac = ESP.getEfuseMac();
  snprintf(m_apSsid, sizeof(m_apSsid), "eink-book-%04x",
           (unsigned)((mac >> 16) & 0xFFFF));   // 后 4 位 MAC，多台设备不撞名

  WiFi.persistent(false);   // 凭据只落我们自己的 NVS，不写 WiFi 驱动的 NVS（防闪存磨损）
  setupHttp();
  if (m_wifiOn) {
    if (m_ssid[0]) startSta();
    else startPortal();     // 开着 WiFi 但从没配过 → 直接进配置热点
    ensureServer();
  }
  Serial.printf("[net] begin: wifi=%d ota=%d up=%d ssid=%s\n",
                m_wifiOn, m_otaOn, m_upOn, m_ssid[0] ? m_ssid : "(none)");
}

void NetSvc::loop() {
  // wifi 保存后的延迟重启：等 WebServer 响应刷出去再动
  if (m_restartAt && (int32_t)(millis() - m_restartAt) >= 0) {
    m_restartAt = 0;
    Serial.println("[net] creds saved -> restart");
    if (Serial) Serial.flush();
    ESP.restart();
  }
  if (!m_wifiOn) return;

  if (m_serverUp) s_srv.handleClient();

  bool sta = WiFi.status() == WL_CONNECTED;
  if (sta) m_staTrying = false;
  // STA 15s 连不上（密码错/路由器不在）→ 回落开配置热点，屏幕菜单里救得回来
  if (m_staTrying && !m_portal && !sta && millis() - m_staTryAt > 15000) {
    Serial.println("[net] STA connect timeout - opening config portal");
    startPortal();
  }
  // OTA 就绪条件：开关开 + 有网口（STA 或热点任一可达即可）
  if (m_otaOn && !m_otaUp && (sta || m_portal)) startOta();
  if (m_otaUp && !m_otaOn) { ArduinoOTA.end(); m_otaUp = false; }
  if (m_otaUp) ArduinoOTA.handle();
}

void NetSvc::beforeSleep() {
  m_restartAt = 0;
  if (m_upActive) {   // 半截上传直接作废，别留垃圾文件
    m_upFile.close();
    SD_MMC.remove(m_upPath);
    m_upActive = false;
  }
  if (m_otaUp) { ArduinoOTA.end(); m_otaUp = false; }
  if (m_serverUp) { s_srv.stop(); m_serverUp = false; }
  if (WiFi.getMode() != WIFI_MODE_NULL) {   // 没起过 WiFi 就别碰驱动
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);                  // wifioff=true：STA + 射频一起关
    WiFi.mode(WIFI_OFF);
  }
  m_portal = false;
  m_staTrying = false;
  if (m_wifiOn) Serial.println("[net] wifi off before deep sleep");
}

// ---- 状态 ----

NetSvc::WifiState NetSvc::state() const {
  if (!m_wifiOn) return OFF;
  if (WiFi.status() == WL_CONNECTED) return CONNECTED;
  if (m_portal) return PORTAL;
  return CONNECTING;
}

const char* NetSvc::ip() {
  IPAddress a;
  if (WiFi.status() == WL_CONNECTED) a = WiFi.localIP();
  else if (m_portal) a = WiFi.softAPIP();
  snprintf(m_ip, sizeof(m_ip), "%s", a.toString().c_str());
  return m_ip;
}

String NetSvc::stateText() const {
  switch (state()) {
    case OFF:       return String(F("关闭"));
    case PORTAL:    return String(F("热点 ")) + m_apSsid;
    case CONNECTING:return String(F("连接 ")) + m_ssid + F(" …");
    default:        return String(F("已连接 ")) + m_ssid;
  }
}

// ---- 菜单动作 ----

void NetSvc::setWifiOn(bool on) {
  m_wifiOn = on;
  s_np.putBool("wifi", on);
  if (on) {
    if (m_ssid[0]) startSta();
    else startPortal();   // 没凭据：开=直接进配置热点
    ensureServer();
    Serial.println("[net] wifi ON");
  } else {
    if (m_otaUp) { ArduinoOTA.end(); m_otaUp = false; }
    if (m_serverUp) { s_srv.stop(); m_serverUp = false; }
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    m_portal = false;
    m_staTrying = false;
    Serial.println("[net] wifi OFF");
  }
}

void NetSvc::setOtaOn(bool on) {
  m_otaOn = on;
  s_np.putBool("ota", on);
  if (!on && m_otaUp) { ArduinoOTA.end(); m_otaUp = false; }
  Serial.printf("[net] OTA %s\n", on ? "ON" : "OFF");
}

void NetSvc::setUpOn(bool on) {
  m_upOn = on;
  s_np.putBool("up", on);
  Serial.printf("[net] book upload service %s\n", on ? "ON" : "OFF");
}

void NetSvc::startPortal() {
  if (!m_wifiOn) { m_wifiOn = true; s_np.putBool("wifi", true); }
  // 有旧凭据就 AP+STA 双模（STA 后台继续试），没有就纯热点
  WiFi.mode(m_ssid[0] ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(m_apSsid);
  m_portal = true;
  ensureServer();
  Serial.printf("[net] portal up: SSID=%s IP=%s\n", m_apSsid,
                WiFi.softAPIP().toString().c_str());
}

void NetSvc::startSta() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(m_ssid, m_pass);
  m_staTrying = true;
  m_staTryAt = millis();
}

void NetSvc::ensureServer() {
  if (m_serverUp) return;
  s_srv.begin();
  m_serverUp = true;
}

void NetSvc::startOta() {
  ArduinoOTA.setHostname(m_apSsid);
  ArduinoOTA.onStart([]() { Serial.println("[ota] start"); });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] done - rebooting"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    static uint8_t bucket = 101;
    uint8_t pct = t ? (uint8_t)((uint64_t)p * 100 / t) : 0;
    if (pct / 5 != bucket / 5) { bucket = pct; Serial.printf("[ota] %u%%\n", pct); }
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
  ArduinoOTA.begin();
  m_otaUp = true;
  Serial.printf("[net] OTA ready: hostname %s.local port 3232 (no password)\n",
                m_apSsid);
}

// ---- HTTP 路由（一次装好，起停由 WiFi 开关管）----

void NetSvc::setupHttp() {
  s_srv.on("/", HTTP_GET, [this]() { handleIndex(); });
  s_srv.on("/wifi", HTTP_GET, [this]() { handleWifiPage(); });
  s_srv.on("/wifi/save", HTTP_POST, [this]() { handleWifiSave(); });
  s_srv.on("/upload", HTTP_GET, [this]() { handleUploadForm(); });
  s_srv.on("/upload", HTTP_POST, [this]() { handleUploadResult(); },
                                 [this]() { uploadChunk(); });
  s_srv.on("/update", HTTP_GET, [this]() { handleUpdateForm(); });
  s_srv.on("/update", HTTP_POST, [this]() { handleUpdateResult(); },
                                 [this]() { updateChunk(); });
  s_srv.onNotFound([]() { s_srv.send(404, "text/plain", "404"); });
}

void NetSvc::handleIndex() {
  String h;
  h.reserve(1200);
  h += F("<h3>eink-book</h3><ul style='list-style:none;padding:0'>");
  h += F("<li>WiFi："); h += stateText(); h += F("</li>");
  h += F("<li>IP："); h += ip(); h += F("</li>");
  h += F("<li>OTA 更新："); h += m_otaOn ? F("开") : F("关"); h += F("</li>");
  h += F("<li>电子书上传："); h += m_upOn ? F("开") : F("关"); h += F("</li>");
  h += F("</ul><p><a href=/wifi>WiFi 配置</a>");
  if (m_upOn) h += F("<a href=/upload>上传电子书</a>");
  if (m_otaOn) h += F("<a href=/update>固件更新</a>");
  h += F("</p>");
  String page = kHead;
  page += h;
  s_srv.send(200, "text/html; charset=utf-8", page);
}

void NetSvc::handleWifiPage() {
  String page;
  page.reserve(1800);
  page += kHead;
  page += F("<h3>WiFi 配置</h3><form method=post action=/wifi/save>"
            "<input name=ssid list=ssids placeholder='WiFi 名称（SSID）' value='");
  htmlEscape(page, String(m_ssid));
  page += F("'><datalist id=ssids>");
  // 阻塞扫描 1-3s：handleClient 在等待窗里跑，翻页顺延即可
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    page += F("<option value='");
    htmlEscape(page, WiFi.SSID(i));
    page += F("'>");
  }
  page += F("</datalist><input type=password name=pass "
            "placeholder='密码（开放网络留空）'>"
            "<button type=submit>保存并重启</button></form>"
            "<p><a href=/>返回</a></p>");
  s_srv.send(200, "text/html; charset=utf-8", page);
}

void NetSvc::handleWifiSave() {
  String s = s_srv.arg("ssid");
  String p = s_srv.arg("pass");
  s.trim();
  if (!s.length() || s.length() > 32 || p.length() > 64) {
    s_srv.send(400, "text/html; charset=utf-8",
               String(kHead) + F("<p>SSID 或密码不合法</p><p><a href=/wifi>返回</a></p>"));
    return;
  }
  snprintf(m_ssid, sizeof(m_ssid), "%s", s.c_str());
  snprintf(m_pass, sizeof(m_pass), "%s", p.c_str());
  s_np.putString("ssid", m_ssid);
  s_np.putString("pass", m_pass);
  Serial.printf("[net] creds saved: ssid=%s\n", m_ssid);
  s_srv.send(200, "text/html; charset=utf-8",
             String(kHead) + F("<p>已保存，设备重启后连接路由器（热点随之关闭）。"
                               "连不上会自动重新开热点。</p><p><a href=/>返回</a></p>"));
  m_restartAt = millis() + 1500;   // 响应发完再重启（loop 里到点执行）
}

// ---- 电子书上传（/upload）----

void NetSvc::handleUploadForm() {
  if (!m_upOn) {
    s_srv.send(403, "text/plain", "upload service is off (menu: 电子书上传服务)");
    return;
  }
  String page;
  page.reserve(900);
  page += kHead;
  page += F("<h3>上传电子书</h3><p>支持 .txt / .epub，写入 SD 卡 <b>/books/</b>"
            "（重名覆盖），完成后自动加入书库。</p>"
            "<form enctype=multipart/form-data method=post>"
            "<input type=file name=file accept='.txt,.epub' required>"
            "<button type=submit>上传</button></form>"
            "<p><a href=/>返回</a></p>");
  s_srv.send(200, "text/html; charset=utf-8", page);
}

void NetSvc::uploadChunk() {
  HTTPUpload& u = s_srv.upload();
  switch (u.status) {
    case UPLOAD_FILE_START: {
      m_upMsg = nullptr;
      m_upActive = false;
      m_upDone = false;
      m_upBytes = 0;
      m_upPath[0] = '\0';
      if (!m_upOn) { m_upMsg = "上传服务未开启"; break; }
      char base[64];
      if (!sanitizeName(u.filename.c_str(), base, sizeof(base)) ||
          !(extIs(base, ".txt") || extIs(base, ".epub"))) {
        m_upMsg = "仅支持 .txt / .epub 文件";
        break;
      }
      if (!Storage::begin()) { m_upMsg = "SD 卡不可用"; break; }
      SD_MMC.mkdir("/books");   // 新布局目录不在时兜底
      snprintf(m_upPath, sizeof(m_upPath), "/books/%s", base);
      m_upFile = SD_MMC.open(m_upPath, FILE_WRITE);   // FILE_WRITE="w"：重名覆盖
      if (!m_upFile) { m_upMsg = "创建文件失败"; m_upPath[0] = '\0'; break; }
      m_upActive = true;
      Serial.printf("[net] upload start: %s\n", m_upPath);
      break;
    }

    case UPLOAD_FILE_WRITE:
      if (!m_upActive) break;
      if (m_upFile.write(u.buf, u.currentSize) != u.currentSize) {
        m_upMsg = "写入失败（SD 卡满或写保护？）";
        m_upFile.close();
        SD_MMC.remove(m_upPath);
        m_upActive = false;
        m_upPath[0] = '\0';
      }
      break;

    case UPLOAD_FILE_END:
      if (!m_upActive) break;
      m_upBytes = u.totalSize;
      m_upFile.close();
      m_upActive = false;
      m_upDone = true;
      break;

    case UPLOAD_FILE_ABORTED:
      if (m_upActive) {
        m_upFile.close();
        SD_MMC.remove(m_upPath);
        m_upActive = false;
        m_upPath[0] = '\0';
      }
      if (!m_upMsg) m_upMsg = "上传中断";
      break;
  }
}

void NetSvc::handleUploadResult() {
  if (m_upMsg) {
    String h = kHead;
    h += F("<p>上传失败：");
    h += m_upMsg;
    h += F("</p><p><a href=/upload>返回</a></p>");
    s_srv.send(400, "text/html; charset=utf-8", h);
    return;
  }
  if (!m_upDone) {
    s_srv.send(400, "text/plain", "no file received");
    return;
  }
  String h = kHead;
  h.reserve(400);
  h += F("<p>上传成功：<b>");
  h += m_upPath;
  h += F("</b>（");
  h += String((unsigned long)m_upBytes);
  h += F(" 字节），已加入书库。</p><p><a href=/>返回</a></p>");
  s_srv.send(200, "text/html; charset=utf-8", h);

  // 回调重扫书库 —— 响应已发出，慢一点无妨
  m_upDone = false;
  if (onBookAdded && m_upPath[0]) {
    char path[96];
    snprintf(path, sizeof(path), "%s", m_upPath);
    m_upPath[0] = '\0';
    onBookAdded(path);
  }
}

// ---- 固件 OTA（/update）----

void NetSvc::handleUpdateForm() {
  if (!m_otaOn) {
    s_srv.send(403, "text/plain", "OTA is off (menu: OTA 更新)");
    return;
  }
  String page;
  page.reserve(900);
  page += kHead;
  page += F("<h3>固件更新 (OTA)</h3>"
            "<p>选择 arduino-cli 编译出的应用镜像 <b>eink.ino.bin</b>"
            "（⚠️ 不是 eink.ino.merged.bin，灌合并镜像会变砖）。更新期间别断电。</p>"
            "<form enctype=multipart/form-data method=post>"
            "<input type=file name=file accept=.bin required>"
            "<button type=submit>更新并重启</button></form>"
            "<p><a href=/>返回</a></p>");
  s_srv.send(200, "text/html; charset=utf-8", page);
}

void NetSvc::updateChunk() {
  HTTPUpload& u = s_srv.upload();
  switch (u.status) {
    case UPLOAD_FILE_START:
      m_updBlocked = false;
      if (!m_otaOn) { m_updBlocked = true; break; }
      Serial.printf("[ota] web update start: %s\n", u.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      break;
    case UPLOAD_FILE_WRITE:
      if (m_updBlocked) break;
      if (Update.write(u.buf, u.currentSize) != u.currentSize)
        Update.printError(Serial);
      break;
    case UPLOAD_FILE_END:
      if (m_updBlocked) break;
      if (!Update.end(true)) Update.printError(Serial);
      break;
    case UPLOAD_FILE_ABORTED:
      if (!m_updBlocked) Update.abort();
      break;
  }
}

void NetSvc::handleUpdateResult() {
  s_srv.sendHeader("Connection", "close");
  // isFinished 兜底：END 没走到（连接半路断）时 _progress<_size，绝不当成功
  bool ok = !m_updBlocked && !Update.hasError() && Update.isFinished();
  if (ok) {
    s_srv.send(200, "text/html; charset=utf-8",
               String(kHead) + F("<p>更新成功，重启中…（屏幕会全刷一次）</p>"));
    Serial.println("[ota] web update ok - restarting");
    if (Serial) Serial.flush();
    delay(400);   // 响应刷出去再动
    ESP.restart();
  }
  // 失败：abort() = _reset() + 记错误 —— 不清的话 _size 残留 >0，
  // 下次 begin() 直接 "already running" 返回 false，网页 OTA 永久卡死
  Update.abort();
  String h = kHead;
  h += F("<p>更新失败：");
  if (m_updBlocked) h += F("OTA 未开启");
  else h += F("详见设备串口日志");
  h += F("</p><p><a href=/update>返回</a></p>");
  s_srv.send(400, "text/html; charset=utf-8", h);
}
