#include "NetSvc.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>
#include <SD_MMC.h>

#include "Storage.h"
#include <esp_system.h>   // esp_reset_reason：/debug 页显示本次复位原因
// 最后 include：Serial 重定向进日志环形缓冲（/debug 网页调试台，DESIGN §11）
#include "DbgLog.h"

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

// URL 百分号编码（UTF-8 字节逐个编；浏览器会再解码回原始文件名）
static String urlEnc(const String& s) {
  static const char h[] = "0123456789ABCDEF";
  String o;
  o.reserve(s.length() * 3);
  for (unsigned i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c > 0x7E || c == '%' || c == '&' || c == '=' || c == '+' ||
        c == '#' || c == '?') {
      o += '%'; o += h[c >> 4]; o += h[c & 15];
    } else o += (char)c;
  }
  return o;
}

// 带 XHR 进度条的上传表单（/upload 与 /update 共用）。
// 原生 form POST 浏览器只给转圈，看不到百分比 —— 改 XHR：
// upload.onprogress 给 百分比/已传大小/速率；onload 显示结果。服务端处理不变。
static String uploadForm(const char* accept, const char* note, const char* btn) {
  String p;
  p.reserve(1600);
  p += F("<form id=u enctype=multipart/form-data method=post>"
         "<input type=file name=file accept='");
  p += accept;
  p += F("' required><button type=submit>");
  p += btn;
  p += F("</button></form>"
         "<div class=bar><i id=p></i></div><pre id=m></pre>"
         "<style>.bar{height:1em;border:1px solid #888;margin:.6em 0}"
         ".bar i{display:block;height:100%;width:0;background:#444}</style>"
         "<script>u.onsubmit=function(e){e.preventDefault();var f=u.file.files[0];"
         "if(!f)return;m.textContent='开始传输…';"
         "var x=new XMLHttpRequest(),t0=Date.now();"
         "x.upload.onprogress=function(e){if(e.lengthComputable){var q=e.loaded/e.total;"
         "p.style.width=(q*100).toFixed(0)+'%';"
         "m.textContent=(q*100).toFixed(0)+'%  '+(e.loaded/1048576).toFixed(1)+' / '"
         "+(e.total/1048576).toFixed(1)+' MB  '+((e.loaded/1048576)/((Date.now()-t0)/1000)).toFixed(2)+' MB/s';}};"
         "x.onload=function(){if(x.status===200){p.style.width='100%';"
         "m.textContent='\\u2714 完成';}else{m.textContent='\\u2718 失败 HTTP '+x.status;}};"
         "x.onerror=function(){m.textContent='\\u2718 网络错误（连接中断？）';};"
         "x.open('POST',location.pathname);x.send(new FormData(u));};</script>"
         "<p>");
  p += note;
  p += F("</p>");
  return p;
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
    m_upFill = 0;
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
  s_srv.on("/library", HTTP_GET, [this]() { handleLibrary(); });
  s_srv.on("/library/del", HTTP_POST, [this]() { handleLibraryDel(); });
  s_srv.on("/library/get", HTTP_GET, [this]() { handleLibraryGet(); });
  s_srv.on("/debug", HTTP_GET, [this]() { handleDebug(); });
  s_srv.on("/debug/log", HTTP_GET, [this]() { handleDebugLog(); });
  s_srv.on("/debug/act", HTTP_POST, [this]() { handleDebugAct(); });
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
  if (m_upOn) h += F("<a href=/upload>上传电子书</a><a href=/library>书库管理</a>");
  if (m_otaOn) h += F("<a href=/update>固件更新</a>");
  h += F("<a href=/debug>调试 /debug</a>");
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
  page.reserve(1600);
  page += kHead;
  page += F("<h3>上传电子书</h3>");
  page += uploadForm(".txt,.epub",
                     "支持 .txt / .epub，写入 SD 卡 /books/（重名覆盖），完成后自动加入书库。"
                     "<a href=/library>书库管理</a> <a href=/>返回</a>",
                     "上传");
  s_srv.send(200, "text/html; charset=utf-8", page);
}

// 512B 对齐分块写：让每次 f_write 恰好一个扇区 → SD 驱动走单块写（CMD24）。
// DESIGN 坑 18 三轮实测（同上电 / 同接线 / 同卡，sdcard/one_bit_write v3）：
//   4KB 多块写(CMD25) 3/3 必挂（0~120KB 间任意一点），512B 单块写 2/3 过 2MB + 读回校验，
//   两者吞吐同为 0.25MB/s → 分块改小没有速度代价，只有稳定性的差别。
// HTTP 分块不是扇区对齐的，所以先在 m_upBuf 里攒够 512B 再整块落盘，
// 文件偏移因此永远是 512 的整倍数，不会触发 FatFS 的读-改-写半个扇区。
// 返回 false = 卡已经写不动（大概率锁死），调用方必须立刻收摊、别再碰 SD。
bool NetSvc::upWrite(const uint8_t* p, size_t n) {
  size_t done = 0;
  while (done < n) {
    size_t take = 512 - m_upFill;
    if (take > n - done) take = n - done;
    memcpy(m_upBuf + m_upFill, p + done, take);
    m_upFill += take;
    done += take;
    if (m_upFill < 512) continue;
    if (m_upFile.write(m_upBuf, 512) != 512) return false;
    m_upFill = 0;
    m_upBytes += 512;
  }
  return true;
}

bool NetSvc::upFlush() {
  if (!m_upFill) return true;
  if (m_upFile.write(m_upBuf, m_upFill) != m_upFill) return false;
  m_upBytes += m_upFill;
  m_upFill = 0;
  return true;
}

// 写失败/中断后的收摊。顺序要紧：先 close 再 recover —— Storage::recover() 会
// end()，句柄开着就 end 是未定义行为；重挂之后删半截文件才有机会删得掉。
// recover 失败就不删了（再 remove 又是一轮 5s 超时），留个 0 字节壳下次覆盖即可。
void NetSvc::upAbort(const char* msg) {
  if (msg) m_upMsg = msg;
  Serial.printf("[net] upload abort: %s，已写 %u 字节（%s）\n", m_upPath,
                (unsigned)m_upBytes, m_upMsg ? m_upMsg : "中断");
  m_upFile.close();                 // 卡死时这一步就会卡满一个 5s 命令超时，认了
  bool ok = Storage::recover();
  if (ok && m_upPath[0]) SD_MMC.remove(m_upPath);
  if (!ok && m_upPath[0])
    m_upMsg = "SD 卡锁死：文件已损坏，请断电重插卡后重传";
  m_upActive = false;
  m_upFill = 0;
  m_upPath[0] = '\0';
}

void NetSvc::uploadChunk() {
  HTTPUpload& u = s_srv.upload();
  switch (u.status) {
    case UPLOAD_FILE_START: {
      m_upMsg = nullptr;
      m_upActive = false;
      m_upDone = false;
      m_upBytes = 0;
      m_upFill = 0;
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
      if (!upWrite(u.buf, u.currentSize))
        upAbort("SD 卡写入失败（卡无应答，疑似锁死）");
      break;

    case UPLOAD_FILE_END: {
      if (!m_upActive) break;
      if (!upFlush()) { upAbort("SD 卡写入失败（卡无应答，疑似锁死）"); break; }
      if (m_upBytes != u.totalSize) {
        upAbort("SD 卡丢数据：写入字节数与上传不符");
        break;
      }
      m_upFile.close();          // 文件 size 只在这一步提交（DESIGN 坑 18）
      // 回读确认真的落盘了，否则页面会报"上传成功"而书库里躺着个 0 字节文件
      m_upFile = SD_MMC.open(m_upPath, FILE_READ);
      size_t sz = m_upFile ? m_upFile.size() : 0;
      if (m_upFile) m_upFile.close();
      if (sz != u.totalSize) {
        upAbort("SD 卡写入未落盘：文件不完整，请断电重插后重传");
        break;
      }
      m_upActive = false;
      m_upDone = true;
      Serial.printf("[net] upload ok: %s (%u bytes)\n", m_upPath, (unsigned)m_upBytes);
      break;
    }

    case UPLOAD_FILE_ABORTED:
      if (m_upActive) upAbort(nullptr);   // 客户端断线：卡多半没事，顺手重挂干净
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
  page.reserve(1600);
  page += kHead;
  page += F("<h3>固件更新 (OTA)</h3>");
  page += uploadForm(".bin",
                     "选择 arduino-cli 编译出的应用镜像 <b>eink.ino.bin</b>"
                     "（⚠️ 不是 eink.ino.merged.bin，灌合并镜像会变砖）。更新期间别断电。"
                     "<a href=/>返回</a>",
                     "更新并重启");
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

// ---- 书库管理（/library：列表 / 下载 / 删除）--------------------------------
// 与 /upload 共用"电子书上传服务"开关（= 网页书库服务总开关）。
// 删除走 NVS 书签清理 + SD remove + 回调 .ino 重扫（可能在读的书被换/被关）。

void NetSvc::handleLibrary() {
  if (!m_upOn) {
    s_srv.send(403, "text/plain", "library service is off (menu: 电子书上传服务)");
    return;
  }
  String page;
  page.reserve(2600);
  page += kHead;
  page += F("<h3>书库管理</h3>");
  if (!Storage::begin()) {
    page += F("<p>SD 卡不可用</p>");
  } else {
    uint64_t tot = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
    page += F("<p>SD 卡："); page += String((unsigned long)(used >> 20));
    page += F(" / "); page += String((unsigned long)(tot >> 20)); page += F(" MB</p><ul>");
    File dir = SD_MMC.open("/books");
    if (dir) {
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;
        const char* n = f.path();
        char base[64];
        if (!n || !sanitizeName(n, base, sizeof(base)) ||
            !(extIs(base, ".txt") || extIs(base, ".epub")))
          continue;
        size_t sz = f.size();
        page += F("<li><b>");
        { String esc; htmlEscape(esc, String(base)); page += esc; }
        page += F("</b> （");
        page += sz >= 1048576 ? String((float)sz / 1048576, 1) + F(" MB")
                              : String((unsigned long)(sz >> 10)) + F(" KB");
        page += F("） <a href='/library/get?f="); page += urlEnc(base);
        page += F("'>下载</a> <button type=button data-n='");
        { String esc; htmlEscape(esc, String(base)); page += esc; }
        page += F("' onclick=\"return del(this)\">删除</button></li>");
      }
      dir.close();
    }
    page += F("</ul><p><a href=/upload>上传电子书</a> <a href=/>返回</a></p>"
              "<script>function del(b){if(!confirm('删除《'+b.dataset.n+'》？"
              "书签与进度一并清除，不可恢复'))return false;"
              "var x=new XMLHttpRequest();x.onload=function(){location.reload()};"
              "x.open('POST','/library/del');"
              "x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
              "x.send('f='+encodeURIComponent(b.dataset.n));return false;}</script>");
  }
  s_srv.send(200, "text/html; charset=utf-8", page);
}

void NetSvc::handleLibraryDel() {
  if (!m_upOn) {
    s_srv.send(403, "text/plain", "library service is off");
    return;
  }
  String f = s_srv.arg("f");
  char base[64];
  if (!sanitizeName(f.c_str(), base, sizeof(base)) ||
      !(extIs(base, ".txt") || extIs(base, ".epub"))) {
    s_srv.send(400, "text/plain", "bad name");
    return;
  }
  if (!Storage::begin()) { s_srv.send(500, "text/plain", "SD unavailable"); return; }
  char path[80];
  snprintf(path, sizeof(path), "/books/%s", base);
  File chk = SD_MMC.open(path, FILE_READ);
  if (!chk) { s_srv.send(404, "text/plain", "not found"); return; }
  chk.close();
  if (!SD_MMC.remove(path)) {
    s_srv.send(500, "text/plain", "remove failed (SD write wedged?)");
    return;
  }
  Serial.printf("[net] library delete: %s\n", path);
  s_srv.send(200, "text/html; charset=utf-8", String(kHead) + F("<p>已删除。</p>"));
  if (onBookRemoved) onBookRemoved(path);
}

void NetSvc::handleLibraryGet() {
  if (!m_upOn) {
    s_srv.send(403, "text/plain", "library service is off");
    return;
  }
  String f = s_srv.arg("f");
  char base[64];
  if (!sanitizeName(f.c_str(), base, sizeof(base)) ||
      !(extIs(base, ".txt") || extIs(base, ".epub"))) {
    s_srv.send(400, "text/plain", "bad name");
    return;
  }
  if (!Storage::begin()) { s_srv.send(500, "text/plain", "SD unavailable"); return; }
  char path[80];
  snprintf(path, sizeof(path), "/books/%s", base);
  File file = SD_MMC.open(path, FILE_READ);
  if (!file) { s_srv.send(404, "text/plain", "not found"); return; }
  s_srv.sendHeader("Content-Disposition",
                   "attachment; filename*=UTF-8''" + urlEnc(base));
  s_srv.streamFile(file, "application/octet-stream");
  file.close();
}

// ---- /debug 网页调试台（DESIGN §11）------------------------------------------
// 屏幕挂了的救砖通道：设备/屏幕/存储状态（经 onDbgInfo 回调从 eink.ino 注入）
// + 复位面板/重画/翻页/重启动作 + 串口日志尾巴（DbgLog 环形缓冲增量拉取，
// /debug/log 2s 一轮）。硬件状态一律不在这边猜，全问 eink.ino。

static const char kBuild[] = __DATE__ " " __TIME__;   // 固件编译时间戳

static const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_EXT:       return "外部复位（EN 键/烧录）";
    case ESP_RST_SW:        return "软件重启";
    case ESP_RST_PANIC:     return "Panic（崩溃）";
    case ESP_RST_INT_WDT:   return "中断看门狗超时";
    case ESP_RST_TASK_WDT:  return "任务看门狗超时";
    case ESP_RST_WDT:       return "看门狗超时";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_BROWNOUT:  return "欠压（Brownout）";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "未知";
  }
}

// /debug 动作按钮（POST 回 /debug/act → 302 回 /debug）。js = 追加的 onsubmit
static String dbgForm(const char* a, const char* label, const char* js = nullptr) {
  String f;
  f.reserve(192);
  f += F("<form method=post action=/debug/act");
  if (js && *js) { f += F(" onsubmit=\""); f += js; f += '"'; }
  f += F("><input type=hidden name=a value=");
  f += a;
  f += F("><button type=submit>");
  f += label;
  f += F("</button></form>");
  return f;
}

void NetSvc::handleDebug() {
  String h;
  h.reserve(3600);
  h += F("<h3>调试台</h3><h4>设备</h4><ul style='list-style:none;padding:0'>");
  uint32_t up = millis() / 1000;
  h += F("<li>运行：");
  if (up >= 3600) { h += String(up / 3600); h += F(" 时 "); }
  h += String((up / 60) % 60); h += F(" 分 "); h += String(up % 60); h += F(" 秒</li>");
  h += F("<li>复位原因："); h += resetReasonText(); h += F("</li>");
  h += F("<li>固件编译："); h += kBuild; h += F("</li>");
  h += F("<li>内存：heap 空闲 "); h += String(ESP.getFreeHeap() >> 10);
  h += F(" KB / 历史最低 "); h += String(ESP.getMinFreeHeap() >> 10);
  h += F(" KB · PSRAM 空闲 "); h += String((unsigned long)(ESP.getFreePsram() >> 20));
  h += F(" MB</li>");
  h += F("<li>WiFi："); h += stateText(); h += F(" · IP "); h += ip();
  if (WiFi.status() == WL_CONNECTED) {
    h += F(" · RSSI "); h += String(WiFi.RSSI()); h += F(" dBm");
  }
  h += F("</li></ul>");

  DbgInfo i = {};
  if (onDbgInfo) onDbgInfo(i);

  h += F("<h4>屏幕</h4><ul style='list-style:none;padding:0'>");
  h += F("<li>模式："); h += i.mode; h += F("</li>");
  h += F("<li>左屏：BUSY=");
  h += i.busyL ? F("HIGH（空闲）") : F("LOW（忙/无应答）");
  h += F(" · "); h += i.leftOk ? F("正常") : F("无响应（已降级，可复位）");
  h += F("</li><li>右屏：BUSY=");
  h += i.busyR ? F("HIGH（空闲）") : F("LOW（忙/无应答）");
  h += F(" · "); h += i.rightOk ? F("正常") : F("无响应（已降级，可复位）");
  h += F("</li>");
  h += F("<li>上次刷新："); h += i.lastTag; h += F(" · "); h += String(i.lastMs);
  h += F(" ms · "); h += i.lastOk ? F("ok") : F("BUSY 超时");
  h += F(" · 累计 "); h += String((unsigned long)i.pageCount); h += F(" 次</li>");
  h += F("<li>统计 fast：n="); h += String((unsigned long)i.fastN);
  h += F(" avg="); h += String((unsigned long)i.fastAvg); h += F(" ms min=");
  h += String((unsigned long)i.fastMin); h += F(" max=");
  h += String((unsigned long)i.fastMax); h += F(" 超时=");
  h += String((unsigned long)i.fastTo); h += F("</li>");
  h += F("<li>统计 full：n="); h += String((unsigned long)i.fullN);
  h += F(" avg="); h += String((unsigned long)i.fullAvg); h += F(" ms min=");
  h += String((unsigned long)i.fullMin); h += F(" max=");
  h += String((unsigned long)i.fullMax); h += F(" 超时=");
  h += String((unsigned long)i.fullTo); h += F("</li></ul>");
  h += F("<div class=row>");
  h += dbgForm("resetL", "复位左屏");
  h += dbgForm("resetR", "复位右屏");
  h += dbgForm("refresh", "重画当前屏");
  h += dbgForm("flipback", "上一对页");
  h += dbgForm("flip", "下一对页");
  h += F("</div>");

  h += F("<h4>存储 / 书</h4><ul style='list-style:none;padding:0'>");
  h += F("<li>SD：");
  if (!i.sdOk) h += F("不可用");
  else {
    h += String(i.sdUsedMb); h += F(" / "); h += String(i.sdTotalMb); h += F(" MB");
  }
  h += F(" · 书库 "); h += String(i.libCount); h += F(" 本</li>");
  h += F("<li>在读：");
  if (i.bookOk) {
    h += F("《"); h += i.bookName; h += F("》 p."); h += String(i.page);
    if (i.pct >= 0) { h += F(" · "); h += String(i.pct); h += F("%"); }
  } else h += F("无（demo 页）");
  h += F("</li></ul>");

  h += F("<h4>按键 / 霍尔</h4><ul style='list-style:none;padding:0'>");
  h += F("<li>霍尔："); h += i.hallText; h += F("</li>");
  h += F("<li>按键：去抖丢弃 "); h += String((unsigned long)i.keysBounce);
  h += F(" · 队列溢出 "); h += String((unsigned long)i.keysOvf); h += F("</li></ul>");

  h += F("<h4>串口日志 <span style='font-weight:normal'>"
         "(滚动缓冲 · 2s 增量 · 与本地串口同一份)</span></h4>"
         "<pre id=log style='height:22em;overflow:auto;background:#f4f4f4;"
         "padding:.6em;font:12px/1.45 monospace;white-space:pre-wrap;"
         "word-break:break-all'></pre>");
  h += F("<div class=row>");
  h += dbgForm("reboot", "重启设备", "return confirm('确定重启设备？')");
  h += F("</div>");
  h += F("<p><a href=/wifi>WiFi</a><a href=/upload>上传</a>"
         "<a href=/library>书库</a><a href=/update>固件</a><a href=/>首页</a></p>");
  // 日志轮询：since = 客户端游标（服务端 X-Seq 回执），textContent 追加不转义
  h += F("<script>var seq=0,t=document.getElementById('log'),busy=false;"
         "function poll(){if(busy)return;busy=true;var x=new XMLHttpRequest();"
         "x.onload=function(){busy=false;if(x.responseText)t.textContent+=x.responseText;"
         "seq=parseInt(x.getResponseHeader('X-Seq')||'0',10);t.scrollTop=t.scrollHeight;"
         "if(t.textContent.length>60000)t.textContent=t.textContent.slice(-48000);};"
         "x.onerror=function(){busy=false;};"
         "x.open('GET','/debug/log?since='+seq);x.send();}"
         "setInterval(poll,2000);poll();</script>");

  String page = kHead;
  page += F("<style>.row{display:flex;flex-wrap:wrap;gap:.4em}"
            ".row form{flex:1 1 7em}input[type=hidden]{display:none}</style>");
  page += h;
  s_srv.send(200, "text/html; charset=utf-8", page);
}

void NetSvc::handleDebugLog() {
  String out;
  uint32_t since = (uint32_t)s_srv.arg("since").toInt();
  uint32_t next = since;
  DbgSerial.dump(out, since, next);
  s_srv.sendHeader("X-Seq", String(next));
  s_srv.send(200, "text/plain; charset=utf-8", out);
}

void NetSvc::handleDebugAct() {
  String a = s_srv.arg("a");
  if (a == "resetL" && onDbgPanel) onDbgPanel(true);
  else if (a == "resetR" && onDbgPanel) onDbgPanel(false);
  else if (a == "refresh" && onDbgRefresh) onDbgRefresh();
  else if (a == "flip" && onDbgFlip) onDbgFlip(1);
  else if (a == "flipback" && onDbgFlip) onDbgFlip(-1);
  else if (a == "reboot") {
    s_srv.send(200, "text/html; charset=utf-8",
               String(kHead) + F("<p>重启中…（屏幕会全刷一次）</p>"));
    Serial.println("[dbg] web: reboot requested");
    m_restartAt = millis() + 1500;   // 响应发完再重启（loop 里到点执行）
    return;
  }
  s_srv.sendHeader("Location", "/debug", true);
  s_srv.send(302, "text/plain", "");
}
