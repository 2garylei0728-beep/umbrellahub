/* 智能旋轉式雨傘寄存櫃 — 在線控制版 v4.2
   硬件：ESP32-E 38pin 擴展板、NEMA17 + TMC2209、WF8006 舵機 x2、操作指示燈 x1
   環境：Arduino IDE 2.x + esp32 開發板套件 2.0.17 + ESP32Servo + WebSockets(Links2004)
   腳位：STEP=26 DIR=27 EN=25 | 舵機 A=32 B=33 | 指示燈=4
   架構：ESP32 連校園 WiFi → WebSocket 中繼伺服器 → 手機網站
         用戶只開網站網址即可控制，無需連接設備熱點；
         本地熱點與序列埠指令保留作除錯／備援。
   網絡：校園 FCT_GUEST 已升級 WPA3-SAE，Arduino WiFi 類別只支援 WPA2，
         故以底層 esp_wifi_set_config 使用 WPA2/WPA3 混合認證（家用網絡同樣兼容）。 */
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <esp_wifi.h>       // WPA3-SAE 底層認證（Arduino WiFi 類別未開放此參數）

/* ===== 腳位 ===== */
#define STEP_PIN 26
#define DIR_PIN 27
#define EN_PIN 25            // TMC2209：低電平 = 啟動
#define SERVO_A_PIN 32
#define SERVO_B_PIN 33
#define LED_PIN 4             // 操作指示燈：GPIO4 → 220Ω 電阻 → LED 長腳(+)；LED 短腳(-) → GND
                             // 備用腳位：5 / 16 / 17(PSRAM 板勿用) / 18 / 19；開發板內置燈多為 GPIO2

/* ===== 可調參數 ===== */
/* --- WiFi：公開版本請在燒錄前填入自己的網絡設定 --- */
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
/* --- 中繼伺服器（部署網站後，把網址貼到這裡；格式：
     wss://主機/ws?role=device&secret=DEVICE_SECRET，secret 必須與伺服器相同）--- */
const char* SERVER_URL = "wss://your-server.example.com/ws?role=device&secret=change-me-device-secret";
/* --- 本地備援熱點（除錯用；用戶控制走網站，不需要連它）--- */
const char* AP_SSID = "Umbrella";
const char* AP_PASS = "12345678";
#define ALWAYS_AP 1          // 1 = 連校園 WiFi 的同時也開本地熱點（備援／除錯）；0 = 只在上網失敗時開
#define ADMIN_KEY "8888"     // 本地管理頁密碼
#define SLOTS 8
#define STEPS_PER_SLOT 400   // 200 步 x 16 細分 / 8 格
#define STEP_DELAY_US 1000   // 每個脈衝的高/低電平時間，太小會失步
#define RAMP_STEPS 80        // 起步與停止的加減速步數
#define RAMP_GAIN_US 15
#define DIR_INVERT false     // 轉向相反時改 true
#define DOOR_OPEN_DEG 78
#define DOOR_CLOSE_DEG 0
#define SERVO_B_MIRROR true  // B 舵機反向安裝；方向相反時改 false
#define DOOR_HOLD_MS 5000    // 開門後等待使用者放/取傘的時間

IPAddress apIP(192, 168, 4, 1);
WebServer server(80);
DNSServer dns;
Preferences prefs;
Servo servoA, servoB;
WebSocketsClient webSocket;

String slotCode[SLOTS];      // 各格取傘碼，"" = 空格
int curSlot = 0;             // 目前對正取傘口的格位（0 起算）
bool busy = false, started = false, noSvc = false, apMode = false;
bool serverOnline = false, serverConfigured = false, inCmd = false;
unsigned long lastTry = 0, lastStatusPush = 0, lastWsTry = 0;
int jobType = 0, jobSlot = 0;  // 1 = 存傘, 2 = 取傘
String lastStatusJson = "";

/* ===== 手機網頁（本地備援用，存放在快閃記憶體）===== */
const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>智能雨傘櫃（本地）</title><style>
body{font-family:sans-serif;margin:0;padding:16px;background:#f4f6f8;color:#1b2733}h1{font-size:20px}
.c{background:#fff;border-radius:12px;padding:14px;margin:12px 0;box-shadow:0 1px 3px #0002}
button{width:100%;padding:14px;font-size:18px;border:0;border-radius:10px;background:#1f6feb;color:#fff;margin-top:8px}
button:disabled{background:#9aa}
input{width:100%;box-sizing:border-box;padding:12px;font-size:20px;text-align:center;border:1px solid #ccd;border-radius:10px;letter-spacing:6px}
#m{font-size:16px;min-height:24px}.big{font-size:40px;letter-spacing:8px;font-weight:700;text-align:center}
</style></head><body><h1>☂️ 智能雨傘寄存櫃（本地備援）</h1>
<div class="c"><div id="st">讀取中…</div></div>
<div class="c"><b>存傘</b><button id="bs" onclick="store()">我要存傘</button></div>
<div class="c"><b>取傘</b><input id="cd" inputmode="numeric" maxlength="4" placeholder="4 位取傘碼">
<button id="bt" onclick="take()">我要取傘</button></div>
<div class="c"><div id="m"></div></div>
<div class="c" style="font-size:13px;color:#667">此為本地備援頁面；正常控制請使用線上網站。</div>
<script>
const $=id=>document.getElementById(id);
let saved='';try{saved=localStorage.getItem('umbrellaCode')||''}catch(e){}
$('cd').value=saved;
async function j(u){const r=await fetch(u);return r.json()}
async function refresh(){try{const s=await j('/api/status');
 $('st').textContent='空格：'+s.free+' / '+s.total+(s.busy?'（櫃子運作中…）':'');
 $('bs').disabled=$('bt').disabled=s.busy;return s}
 catch(e){$('st').textContent='連線中斷'}}
async function wait(){for(let i=0;i<60;i++){const s=await refresh();if(s&&!s.busy)return;
 await new Promise(r=>setTimeout(r,1000))}}
async function store(){$('m').textContent='請稍候…';const r=await j('/api/store');
 if(!r.ok){$('m').textContent=r.msg;return}
 try{localStorage.setItem('umbrellaCode',r.code)}catch(e){}
 $('cd').value=r.code;
 $('m').innerHTML='第 '+r.slot+' 格開門中，請放入雨傘<br>你的取傘碼<div class="big">'+r.code+'</div>請截圖或抄下';
 await wait();$('m').innerHTML+='<br>✅ 完成，門已關閉'}
async function take(){const c=$('cd').value.trim();$('m').textContent='請稍候…';
 const r=await j('/api/take?code='+encodeURIComponent(c));
 if(!r.ok){$('m').textContent=r.msg;return}
 try{localStorage.removeItem('umbrellaCode')}catch(e){}
 $('m').textContent='第 '+r.slot+' 格開門中，請取走雨傘';
 await wait();$('m').textContent='✅ 完成，歡迎再用';$('cd').value=''}
refresh();setInterval(refresh,4000);
</script></body></html>
)rawliteral";

/* ===== 操作指示燈（非阻塞閃爍，不影響馬達與網頁回應）===== */
/* 信號設計（一顆燈區分存傘／取傘，持續到完成）：
   存傘中  慢閃：亮 0.3s / 滅 0.3s（轉動馬達期間）
   取傘中  快閃：亮 0.1s / 滅 0.1s
   開門中  常亮（門已開，請放傘／請取傘）
   完成    三連快閃（每次約 0.2s）後熄滅，回到待機
   待機    全部正常：每 3 秒輕閃一下
           中繼離線：每 3 秒雙閃（WiFi 有連但網站控制不通）
           WiFi 離線：每 1 秒快閃警示                                  */
int ledMode = 5;               // 0=滅 1=存傘中 2=取傘中 3=開門等待 4=完成(一次性) 5=待機
int ledBlinks = 0;             // 一次性閃爍剩餘次數（0 = 無）
int ledBlinkMs = 200;          // 一次性閃爍半週期
unsigned long ledT0 = 0;       // 一次性閃爍起點

void ledWrite(bool on) { digitalWrite(LED_PIN, on ? HIGH : LOW); }

void ledSetMode(int m) {       // 設定持續信號（會取消進行中的一次性閃爍）
  ledBlinks = 0;
  if (ledMode != m) { ledMode = m; ledTick(); }
}

void ledFlash(int times, int halfMs) {   // 一次性閃爍（完成提示）
  ledBlinks = times; ledBlinkMs = halfMs; ledT0 = millis();
}

void ledTick() {
  unsigned long now = millis();
  if (ledBlinks > 0) {                       // 一次性閃爍進行中
    int idx = (now - ledT0) / ledBlinkMs;
    if (idx >= ledBlinks) { ledBlinks = 0; ledTick(); return; }   // 閃完回歸持續信號
    ledWrite(idx % 2 == 0);
    return;
  }
  bool on = false;
  switch (ledMode) {
    case 1: on = (now % 600) < 300; break;   // 存傘：慢閃
    case 2: on = (now % 200) < 100; break;   // 取傘：快閃
    case 3: on = true; break;                // 開門等待：常亮
    case 5:                                  // 待機：三態指示
      if (WiFi.status() != WL_CONNECTED) on = (now % 1000) < 100;                    // WiFi 離線：快閃
      else if (serverConfigured && !serverOnline) { unsigned long p = now % 3000; on = p < 60 || (p >= 150 && p < 210); }  // 中繼離線：雙閃
      else on = (now % 3000) < 60;                                                    // 全部正常：輕閃
      break;
    default: on = false; break;              // 0=滅；4=完成（只用一次性閃爍呈現）
  }
  ledWrite(on);
}

/* ===== 基礎工具 ===== */
void service() {
  if (!started) return;
  ledTick();
  if (apMode) dns.processNextRequest();
  server.handleClient();
  webSocket.loop();
}
void waitMs(unsigned long ms) { unsigned long t = millis(); while (millis() - t < ms) { service(); delay(5); } }

String ipStr() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (apMode) return String("本地熱點 192.168.4.1");
  return String("未連線");
}

/* ===== WiFi：AP（備援）＋ STA（校園網絡） ===== */
/* WiFi 事件日誌：連線診斷用（原因碼 15 = 認證/密碼失敗，2 = 認證失效，6 → WPA3-SAE 協商成功） */
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[WiFi] 已連上 AP（%s）\n", (const char*)info.wifi_sta_connected.ssid);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi] 與 AP 斷開，原因碼：%d\n", info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      Serial.printf("[WiFi] 認證模式變更：%d → %d（6 = WPA3-SAE）\n",
                    info.wifi_sta_authmode_change.old_mode, info.wifi_sta_authmode_change.new_mode);
      break;
    default: break;
  }
}

void startAP() {
  apMode = true;
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);          // 頻道 1、不隱藏、最多 4 台
  dns.start(53, "*", apIP);                        // 所有網址導向本機（強制門戶）
}

/* 以指定認證模式連線：覆寫底層 STA 設定（WiFi.begin 只會用 WPA2-PSK）。
   注意：threshold 是掃描過濾器的安全級別下限，第一次嘗試必須是 WPA2_PSK，
   否則會把純 WPA2 的家用／熱點網絡過濾掉（設成 WPA3 級別是常見誤區）。 */
bool wifiConnectWith(wifi_auth_mode_t auth, wifi_sae_pwe_method_t pwe, bool pmfRequired) {
  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  strncpy((char*)conf.sta.ssid, WIFI_SSID, sizeof(conf.sta.ssid) - 1);
  strncpy((char*)conf.sta.password, WIFI_PASS, sizeof(conf.sta.password) - 1);
  conf.sta.threshold.rssi = -127;               // 不設訊號過濾（與 Arduino 預設一致）
  conf.sta.threshold.authmode = auth;           // 掃描安全下限（WPA2_PSK = 接受 WPA2/WPA3/過渡）
  conf.sta.sae_pwe_h2e = pwe;                   // 校園 FCT_GUEST 為 WPA3-H2E 強制模式
  conf.sta.pmf_cfg.capable = true;              // WPA3 需要管理幀保護
  conf.sta.pmf_cfg.required = pmfRequired;      // H2E 網絡要求 PMF；家用 WPA2 網絡不可設 true
  if (esp_wifi_set_config(WIFI_IF_STA, &conf) != ESP_OK) return false;
  esp_wifi_connect();
  return true;
}

bool connectWiFi() {
  WiFi.setHostname("umbrella");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  /* 多輪嘗試（每輪重新掃描選 AP）：
     家用／熱點 WPA2 網絡第一輪即連上；
     校園 FCT_GUEST 多個 AP 安全配置不一（部分 WPA2/WPA3 過渡、部分 WPA3-H2E only），
     換 AP 重試有機會連上；最壞情況 4 輪約 60 秒，之後由 loop 每 15 秒持續重試。 */
  for (int round = 1; round <= 4 && WiFi.status() != WL_CONNECTED; round++) {
    if (round > 1) { Serial.printf("第 %d 輪重試 ", round); WiFi.disconnect(); delay(500); }
    wifiConnectWith(WIFI_AUTH_WPA2_PSK, WPA3_SAE_PWE_BOTH, false);         // WPA2-PSK（與 Arduino 預設相同）
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) { delay(300); ledTick(); Serial.print("."); }
    if (WiFi.status() != WL_CONNECTED) {        // 混合失敗：純 WPA3-SAE（H2E 強制）
      Serial.println();
      Serial.print("  改用 WPA3-SAE(H2E) ");
      WiFi.disconnect(); delay(300);            // 先斷開，否則 esp_wifi_connect 可能報「sta is connecting」而不生效
      wifiConnectWith(WIFI_AUTH_WPA3_PSK, WPA3_SAE_PWE_HASH_TO_ELEMENT, true);
      t = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t < 6000) { delay(300); ledTick(); Serial.print("."); }
    }
    Serial.println();
  }
  return WiFi.status() == WL_CONNECTED;
}

/* ===== 中繼伺服器（WebSocket 客戶端） ===== */
struct WsTarget { String host; int port; String path; bool ssl; };
WsTarget wsTgt;             // 避免自訂型別作為回傳值（Arduino 原型插入限制）

void parseWsUrl(const String& url) {
  String u = url;
  if (u.startsWith("wss://")) { wsTgt.ssl = true; u = u.substring(6); }
  else if (u.startsWith("ws://"))  { wsTgt.ssl = false; u = u.substring(5); }
  else { wsTgt.ssl = false; }
  int slash = u.indexOf('/');
  String hostport = slash < 0 ? u : u.substring(0, slash);
  wsTgt.path = slash < 0 ? String("/") : u.substring(slash);
  int colon = hostport.indexOf(':');
  if (colon >= 0) { wsTgt.host = hostport.substring(0, colon); wsTgt.port = hostport.substring(colon + 1).toInt(); }
  else { wsTgt.host = hostport; wsTgt.port = wsTgt.ssl ? 443 : 80; }
}

void pushResult(bool ok, const String& msg, int slot = -1, const String& code = "") {
  if (!serverOnline) return;
  String j = String("{\"t\":\"result\",\"ok\":") + (ok ? "true" : "false") +
             ",\"msg\":\"" + msg + "\"";
  if (slot > 0) j += ",\"slot\":" + String(slot);
  if (code != "") j += ",\"code\":\"" + code + "\"";
  j += "}";
  webSocket.sendTXT(j.c_str());
}

void pushStatus(bool force) {
  if (!serverOnline) return;
  String codes = "[";
  for (int i = 0; i < SLOTS; i++) {
    codes += "\"" + slotCode[i] + "\"";
    if (i < SLOTS - 1) codes += ",";
  }
  codes += "]";
  String j = String("{\"t\":\"status\",\"free\":") + freeCount() +
             ",\"busy\":" + ((busy || jobType) ? "true" : "false") +
             ",\"cur\":" + String(curSlot + 1) +
             ",\"codes\":" + codes + "}";
  if (force || (j != lastStatusJson && millis() - lastStatusPush > 300) || millis() - lastStatusPush > 2000) {
    webSocket.sendTXT(j.c_str());
    lastStatusJson = j;
    lastStatusPush = millis();
  }
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      serverOnline = false;
      Serial.println("[WS] 與中繼伺服器斷開，將自動重連");
      break;
    case WStype_CONNECTED:
      serverOnline = true;
      Serial.println("[WS] 已連接中繼伺服器，在線控制就緒");
      webSocket.sendTXT("{\"t\":\"hello\",\"fw\":\"v4.2-online\"}");
      pushStatus(true);
      break;
    case WStype_TEXT:
      handleServerCmd(String((const char*)payload).substring(0, length));
      break;
    default: break;
  }
}

void wsTick() {
  if (!serverConfigured) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!serverOnline && millis() - lastWsTry > 5000) {
    lastWsTry = millis();
    parseWsUrl(String(SERVER_URL));
    Serial.printf("[WS] 連接伺服器 %s:%d%s ...\n", wsTgt.host.c_str(), wsTgt.port, wsTgt.path.c_str());
    if (wsTgt.ssl) webSocket.beginSSL(wsTgt.host.c_str(), wsTgt.port, wsTgt.path.c_str());
    else webSocket.begin(wsTgt.host.c_str(), wsTgt.port, wsTgt.path.c_str());
  }
}

/* 來自網站的指令（純文字協議，避免在 ESP32 上解析 JSON） */
void handleServerCmd(const String& raw) {
  if (inCmd) return;                       // 防止重入
  inCmd = true;
  String m = raw; m.trim();
  if (m == "status") {
    pushStatus(true);
  }
  else if (m == "store") {
    if (busy || jobType) pushResult(false, "櫃子運作中，請稍後再試");
    else {
      int k = -1;
      for (int i = 0; i < SLOTS; i++) if (slotCode[i] == "") { k = i; break; }
      if (k < 0) pushResult(false, "櫃子已滿");
      else {
        String code = newCode();
        slotCode[k] = code; jobType = 1; jobSlot = k;
        ledSetMode(1);                       // 指示燈：存傘慢閃，直到完成
        pushResult(true, "", k + 1, code);
        pushStatus(true);
      }
    }
  }
  else if (m.startsWith("take ")) {
    String code = m.substring(5); code.trim();
    if (busy || jobType) pushResult(false, "櫃子運作中，請稍後再試");
    else {
      int k = -1;
      if (code != "") for (int i = 0; i < SLOTS; i++) if (slotCode[i] == code) { k = i; break; }
      if (k < 0) pushResult(false, "取傘碼錯誤");
      else { jobType = 2; jobSlot = k; ledSetMode(2); pushStatus(true); }   // 指示燈：取傘快閃
    }
  }
  else if (m == "open") {
    if (busy || jobType) pushResult(false, "櫃子運作中");
    else { setDoor(true);  pushResult(true, "門已開"); }
  }
  else if (m == "close") {
    if (busy || jobType) pushResult(false, "櫃子運作中");
    else { setDoor(false); pushResult(true, "門已關"); }
  }
  else if (m.startsWith("home ")) {
    int n = m.substring(5).toInt();
    if (n >= 1 && n <= SLOTS) { curSlot = n - 1; saveCur(); pushResult(true, "已對位：第 " + String(n) + " 格"); pushStatus(true); }
    else pushResult(false, "格號必須是 1-" + String(SLOTS));
  }
  else if (m.startsWith("go ")) {
    int n = m.substring(3).toInt();
    if (busy || jobType) pushResult(false, "櫃子運作中");
    else if (n >= 1 && n <= SLOTS) { rotateTo(n - 1); pushResult(true, "已轉到第 " + String(n) + " 格"); pushStatus(true); }
    else pushResult(false, "格號必須是 1-" + String(SLOTS));
  }
  else if (m == "clear") {
    for (int i = 0; i < SLOTS; i++) { slotCode[i] = ""; saveSlot(i); }
    pushResult(true, "已清空全部格位");
    pushStatus(true);
  }
  inCmd = false;
}

/* ===== 資料儲存（Preferences，斷電不遺失）===== */
void saveSlot(int i) {
  char k[6]; snprintf(k, sizeof(k), "s%d", i);
  if (slotCode[i] == "") prefs.remove(k); else prefs.putString(k, slotCode[i]);
}
void saveCur() { prefs.putInt("cur", curSlot); }
void loadAll() {
  curSlot = prefs.getInt("cur", 0);
  for (int i = 0; i < SLOTS; i++) { char k[6]; snprintf(k, sizeof(k), "s%d", i); slotCode[i] = prefs.getString(k, ""); }
}
int freeCount() { int n = 0; for (int i = 0; i < SLOTS; i++) if (slotCode[i] == "") n++; return n; }
String newCode() {
  while (true) {
    String c = String((int)random(1000, 10000));
    bool dup = false;
    for (int i = 0; i < SLOTS; i++) if (slotCode[i] == c) dup = true;
    if (!dup) return c;
  }
}

/* ===== 步進馬達（TMC2209 STEP/DIR/EN）===== */
void motorOn()  { digitalWrite(EN_PIN, LOW); delay(2); }
void motorOff() { digitalWrite(EN_PIN, HIGH); }   // 斷電：不鎖軸、不發熱
void moveSteps(long steps, int dir) {
  bool fwd = (dir > 0) != DIR_INVERT;
  digitalWrite(DIR_PIN, fwd ? HIGH : LOW);
  delayMicroseconds(20);
  for (long i = 0; i < steps; i++) {
    long edge = min(i, steps - 1 - i);
    int extra = edge < RAMP_STEPS ? (RAMP_STEPS - edge) * RAMP_GAIN_US : 0;
    digitalWrite(STEP_PIN, HIGH); delayMicroseconds(STEP_DELAY_US + extra);
    digitalWrite(STEP_PIN, LOW);  delayMicroseconds(STEP_DELAY_US + extra);
    if (i % 40 == 0) service();                    // 轉動時仍回應網頁與中繼
  }
}
void rotateTo(int target) {
  int diff = target - curSlot;
  if (diff > SLOTS / 2) diff -= SLOTS;             // 取最短路徑
  if (diff < -SLOTS / 2) diff += SLOTS;
  if (diff == 0) return;
  motorOn();
  moveSteps((long)abs(diff) * STEPS_PER_SLOT, diff > 0 ? 1 : -1);
  motorOff();
  curSlot = target; saveCur();
}

/* ===== 門片 ===== */
void setDoor(bool open) {
  int a = open ? DOOR_OPEN_DEG : DOOR_CLOSE_DEG;
  servoA.write(a);
  servoB.write(SERVO_B_MIRROR ? 180 - a : a);
  waitMs(700);
}

/* ===== 存/取傘流程（在 loop 中執行）===== */
void runJob() {
  busy = true;
  int t = jobType, s = jobSlot; jobType = 0;
  pushStatus(true);
  rotateTo(s);
  setDoor(true);
  ledSetMode(3);                 // 開門等待：常亮（請放傘／請取傘）
  waitMs(DOOR_HOLD_MS);
  setDoor(false);
  if (t == 2) slotCode[s] = "";
  saveSlot(s);
  busy = false;
  ledSetMode(5);                 // 先回待機，再觸發完成閃爍（順序不可顛倒，否則閃爍被取消）
  ledFlash(3, 200);              // 完成：三連快閃
  pushStatus(true);
}

/* ===== 本地網頁 API（備援／除錯用）===== */
void sendJson(const String& s) { server.send(200, "application/json; charset=utf-8", s); }
void handleRoot() { server.send_P(200, "text/html; charset=utf-8", PAGE); }
void apiStatus() {
  sendJson(String("{\"free\":") + freeCount() + ",\"total\":" + SLOTS + ",\"busy\":" + ((busy || jobType) ? "true" : "false") + "}");
}
void apiStore() {
  if (busy || jobType) { sendJson("{\"ok\":false,\"msg\":\"櫃子運作中，請稍後再試\"}"); return; }
  int k = -1;
  for (int i = 0; i < SLOTS; i++) if (slotCode[i] == "") { k = i; break; }
  if (k < 0) { sendJson("{\"ok\":false,\"msg\":\"櫃子已滿\"}"); return; }
  String code = newCode();
  slotCode[k] = code; jobType = 1; jobSlot = k;
  ledSetMode(1);                       // 指示燈：存傘慢閃（本地備援路徑）
  sendJson(String("{\"ok\":true,\"slot\":") + (k + 1) + ",\"code\":\"" + code + "\"}");
}
void apiTake() {
  if (busy || jobType) { sendJson("{\"ok\":false,\"msg\":\"櫃子運作中，請稍後再試\"}"); return; }
  String code = server.arg("code"); code.trim();
  int k = -1;
  if (code != "") for (int i = 0; i < SLOTS; i++) if (slotCode[i] == code) { k = i; break; }
  if (k < 0) { sendJson("{\"ok\":false,\"msg\":\"取傘碼錯誤\"}"); return; }
  jobType = 2; jobSlot = k;
  ledSetMode(2);                       // 指示燈：取傘快閃（本地備援路徑）
  sendJson(String("{\"ok\":true,\"slot\":") + (k + 1) + "}");
}

/* ===== 本地管理頁（備援）===== */
void handleAdmin() {
  if (server.arg("key") != ADMIN_KEY) { server.send(403, "text/plain", "forbidden"); return; }
  String act = server.arg("act"), msg = "";
  int n = server.arg("n").toInt();
  noSvc = true;
  if (act != "" && (busy || jobType)) msg = "櫃子運作中";
  else if (act == "home" && n >= 1 && n <= SLOTS) { curSlot = n - 1; saveCur(); msg = "已對位：第 " + String(n) + " 格在取傘口"; }
  else if (act == "go" && n >= 1 && n <= SLOTS) { rotateTo(n - 1); msg = "已轉到第 " + String(n) + " 格"; }
  else if (act == "clear") { for (int i = 0; i < SLOTS; i++) { slotCode[i] = ""; saveSlot(i); } msg = "已清空全部格位"; }
  else if (act == "open") { setDoor(true); msg = "門已開"; }
  else if (act == "close") { setDoor(false); msg = "門已關"; }
  noSvc = false;
  String base = "/admin?key=" + String(ADMIN_KEY) + "&act=";
  String h = "<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:sans-serif;padding:14px'><h2>管理頁（本地）</h2><p>" + msg + "</p><p>連線：" + ipStr() + "</p><p>取傘口對正：第 " + String(curSlot + 1) + " 格</p><table border=1 cellpadding=6>";
  for (int i = 0; i < SLOTS; i++) { String cell = slotCode[i] == "" ? String("空") : slotCode[i]; h += "<tr><td>第 " + String(i + 1) + " 格</td><td>" + cell + "</td></tr>"; }
  h += "</table><p><a href='" + base + "open'>開門</a> | <a href='" + base + "close'>關門</a> | <a href='" + base + "clear'>清空全部</a></p><p>對位：";
  for (int i = 1; i <= SLOTS; i++) h += "<a href='" + base + "home&n=" + String(i) + "'>" + String(i) + "</a> ";
  h += "</p><p>轉到：";
  for (int i = 1; i <= SLOTS; i++) h += "<a href='" + base + "go&n=" + String(i) + "'>" + String(i) + "</a> ";
  h += "</p></body>";
  server.send(200, "text/html; charset=utf-8", h);
}

/* ===== 序列埠指令（除錯用，速率 115200）===== */
const char* ledName() {
  if (ledBlinks > 0) return "完成閃爍";
  switch (ledMode) {
    case 1: return "存傘慢閃";
    case 2: return "取傘快閃";
    case 3: return "開門常亮";
    case 5: return "待機";
    default: return "熄滅";
  }
}

void handleSerial() {
  String c = Serial.readStringUntil('\n'); c.trim();
  if (c == "status") {
    Serial.println("連線：" + ipStr() + (serverOnline ? " ＋ 中繼伺服器在線" : "（中繼離線）"));
    Serial.printf("取傘口對正：第 %d 格，空格 %d/%d\n", curSlot + 1, freeCount(), SLOTS);
    Serial.printf("指示燈：%s\n", ledName());
    for (int i = 0; i < SLOTS; i++) Serial.printf("  第 %d 格：%s\n", i + 1, slotCode[i] == "" ? "(空)" : slotCode[i].c_str());
  }
  else if (c == "led test") {           // 指示燈測試：依序示範全部信號（約 7 秒）
    Serial.println("指示燈測試 → 存傘慢閃");   ledSetMode(1); waitMs(2000);
    Serial.println("指示燈測試 → 取傘快閃");   ledSetMode(2); waitMs(2000);
    Serial.println("指示燈測試 → 開門常亮");   ledSetMode(3); waitMs(2000);
    Serial.println("指示燈測試 → 完成三連閃"); ledSetMode(5); ledFlash(3, 200); waitMs(1200);
    Serial.println("指示燈測試完成，回到待機");
  }
  else if (c == "scan") {               // 掃描附近網絡（認證碼：3=WPA2 4=WPA/WPA2 6=WPA3 7=WPA2/WPA3過渡）
    int n = WiFi.scanNetworks();
    Serial.printf("掃描到 %d 個網絡：\n", n);
    for (int i = 0; i < n; i++)
      Serial.printf("  %-22s 強度:%4d 頻道:%2d 認證:%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i));
    WiFi.scanDelete();
  }
  else if (c == "open") setDoor(true);
  else if (c == "close") setDoor(false);
  else if (c.startsWith("step ")) { long n = c.substring(5).toInt(); motorOn(); moveSteps(labs(n), n > 0 ? 1 : -1); motorOff(); Serial.println("完成"); }
  else if (c.startsWith("go ")) { int n = c.substring(3).toInt(); if (n >= 1 && n <= SLOTS) { rotateTo(n - 1); Serial.println("完成"); } }
  else if (c.startsWith("home ")) { int n = c.substring(5).toInt(); if (n >= 1 && n <= SLOTS) { curSlot = n - 1; saveCur(); Serial.println("已對位"); } }
  else if (c == "clear") { for (int i = 0; i < SLOTS; i++) { slotCode[i] = ""; saveSlot(i); } Serial.println("已清空"); }
  else Serial.println("指令：status / open / close / step <步數> / go <1-8> / home <1-8> / clear / led test / scan");
}

void setup() {
  Serial.begin(115200); delay(300);
  Serial.println("\n===== 智能旋轉式雨傘寄存櫃 啟動（在線控制版 v4.2）=====");
  WiFi.onEvent(onWiFiEvent);                 // WiFi 事件日誌（連線診斷）
  pinMode(STEP_PIN, OUTPUT); pinMode(DIR_PIN, OUTPUT); pinMode(EN_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);   // 指示燈預設熄滅
  digitalWrite(EN_PIN, HIGH);                      // 開機先斷電
  ledSetMode(5);                                   // 待機信號：在線每 3 秒輕閃
  servoA.setPeriodHertz(50); servoB.setPeriodHertz(50);
  servoA.attach(SERVO_A_PIN, 500, 2500); servoB.attach(SERVO_B_PIN, 500, 2500);
  prefs.begin("umbrella", false); loadAll();
  setDoor(false);

#if ALWAYS_AP
  WiFi.mode(WIFI_AP_STA);                          // 本地熱點 + 校園 WiFi 同時在線
  startAP();
  Serial.printf("本地備援熱點：%s （%s）\n", AP_SSID, apIP.toString().c_str());
  Serial.print("連線到校園 WiFi：" + String(WIFI_SSID) + " ");
#else
  WiFi.mode(WIFI_STA);
  Serial.print("連線到校園 WiFi：" + String(WIFI_SSID) + " ");
#endif
  if (connectWiFi()) {
    Serial.printf("成功！IP：%s（備用 http://umbrella.local）\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("umbrella")) MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("失敗");
#if !ALWAYS_AP
    startAP();                                     // 上網失敗才開本地熱點
    Serial.printf("本地備援熱點：%s 網址：http://192.168.4.1\n", AP_SSID);
#endif
  }

  /* 本地備援網頁（調試用） */
  server.on("/", handleRoot);
  server.on("/api/status", apiStatus);
  server.on("/api/store", apiStore);
  server.on("/api/take", apiTake);
  server.on("/admin", handleAdmin);
  server.onNotFound([]() { if (apMode) { server.sendHeader("Location", "http://192.168.4.1/", true); server.send(302, "text/plain", ""); } else server.send(404, "text/plain", "Not found"); });
  server.begin(); started = true;

  /* 中繼伺服器（在線控制） */
  String srv = String(SERVER_URL);
  serverConfigured = (srv.length() > 0 && srv.indexOf("your-server") < 0);
  if (serverConfigured) {
    Serial.println("中繼伺服器：" + srv);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    wsTick();
  } else {
    Serial.println("尚未設定中繼伺服器網址（SERVER_URL），在線控制未啟用；本地備援正常");
  }
  Serial.printf("操作指示燈：GPIO%d（存傘慢閃／取傘快閃／開門常亮／完成三連閃；序列埠 led test 可測試）\n", LED_PIN);
  Serial.println("注意：本版無歸零感測器，轉盤被手動轉動後請用 home <格號> 重新對位");
}

void loop() {
  service();
  wsTick();
  if (jobType) runJob();
  if (serverOnline) pushStatus(false);
  if (Serial.available()) handleSerial();
  if (WiFi.status() != WL_CONNECTED && millis() - lastTry > 15000) { lastTry = millis(); WiFi.reconnect(); }
  delay(2);
}
