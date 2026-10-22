/* 智能旋轉式雨僘寄存櫃 — 在線控制版 v4.2
   硬件：ESP32-E 38pin 擴展板、NEMA17 + TMC2209、WF8006 舵機 x2、操作指示燈 x1
   環境：Arduino IDE 2.x + esp32 開發板套件 3.x（實測 3.3.12 / IDF 5.5）
         + ESP32Servo 3.2.1 + WebSockets 2.7.2
         （教學指定 2.0.17 亦可編譯，但 IDF 4.4 的 WPA3-H2E 不完整，連不上校園 FCT_GUEST；
           兩套工具鏈互相獨立，詳見同目錄 README.md 的「3.x 工具鏈安裝記錄」）
   腳位：STEP=26 DIR=27 EN=25 | 舵機 A=32 B=33 | 指示燈=4
   架構：ESP32 連校園 WiFi → WebSocket 中繼伺服器 → 手機網站
         用戶只開網站網址即可控制，無需連接設備熱點；
         本地熱點與序列埠指令保留作除錯／備援。
   網絡：校園 FCT_GUEST 為 WPA3-個人（H2E 強制），Arduino WiFi 類別只支援 WPA2，
         故以底層 esp_wifi_set_config 使用 WPA2/WPA3 混合認證（家用網絡同樣兼容）；
         實測 3.x（IDF 5.1+）可直接連上 FCT_GUEST，2.0.17（IDF 4.4）認證被拒（原因碼 202）。 */
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
/* --- 校園 WiFi --- */
const char* WIFI_SSID = "FCT_GUEST";
const char* WIFI_PASS = "fct28831980";
/* --- 中繼伺服器（部署網站後，把網址貼到這裡；格式：
     wss://主機/ws?role=device&secret=DEVICE_SECRET，secret 必須與伺服器相同）--- */
const char* SERVER_URL = "wss://umbrellahub-caozd3vf.manus.space/ws?role=device&secret=T3pVK7eNVgl_hJEuZCE4XxdL1c4AkXM6";
/* --- 本地備援熱點（除錯用；用戶控制走網站，不需要連它）--- */
const char* AP_SSID = "Umbrella";
const char* AP_PASS = "12345678";
#define ALWAYS_AP 1          // 1 = 連校園 WiFi 的同時也開本地熱點（備援／除錯）；0 = 只在上網失敗時開
#define ADMIN_KEY "8888"     // 本地管理頁密碼
#define SLOTS 8
#define STEPS_PER_SLOT 400   // 200 步 x 16 細分 / 8 格
#define STEP_DELAY_US 800    // 每個脈衝的高/低電平時間（800us 實測不失步；再小有風險）
#define RAMP_STEPS 80        // 起步與停止的加減速步數
#define RAMP_GAIN_US 15
#define DIR_INVERT false     // 轉向相反時改 true
#define DOOR_OPEN_DEG 78
#define DOOR_CLOSE_DEG 0
#define SERVO_B_MIRROR true  // B 舵機反向安裝；方向相反時改 false
#define DOOR_HOLD_MS 3500    // 開門後等待使用者放/取僘的時間（3500 實測足夠，縮短指令周期）

IPAddress apIP(192, 168, 4, 1);
WebServer server(80);
DNSServer dns;
Preferences prefs;
Servo servoA, servoB;
WebSocketsClient webSocket;

String slotCode[SLOTS];      // 各格取僘碼，"" = 空格
int curSlot = 0;             // 目前對正取僘口的格位（0 起算）
bool busy = false, started = false, noSvc = false, apMode = false;
bool serverOnline = false, serverConfigured = false, inCmd = false;
unsigned long lastTry = 0, lastStatusPush = 0, lastWsTry = 0;
int wsFailStreak = 0;            // 連續 WS 連接失敗計數（自癒用）
int wifiFail = 0;                // 連續 WiFi 重連失敗計數（退避用）
uint8_t staMac[6] = {0};         // 自訂 STA MAC（被 AP 拉黑時更換用；全 0 = 用原廠 MAC）
unsigned long lastWifiTry = 0;   // 上次 WiFi 重連嘗試時間
String cfgSsid, cfgPass;       // 從 Preferences 讀取的 WiFi 憑證（配網用，空發時回落到寫死預設）
int jobType = 0, jobSlot = 0;  // 1 = 存僘, 2 = 取僘
String lastStatusJson = "";

/* ===== 手機網頁（本地備援用，存放在快閃記憶體）===== */
const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>智能雨&#20438;寄存櫃 &#183; 本地控制</title><style>
:root{--bg:#f4f6f8;--card:#fff;--txt:#1b2733;--mut:#5a6b7b;--pri:#1f6feb;--ok:#1d7a1d;--err:#b3261e;--line:#dde3ea}
@media(prefers-color-scheme:dark){:root{--bg:#0f151c;--card:#1a232e;--txt:#e8eef4;--mut:#93a3b3;--pri:#4d8ef7;--ok:#6bd06b;--err:#ff9e94;--line:#2c3a48}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{font-family:-apple-system,BlinkMacSystemFont,"Ping Fang TC","Noto Sans TC","Microsoft YaHei",sans-serif;margin:0;padding:calc(14px+env(safe-area-inset-top)) 16px calc(16px+env(safe-area-inset-bottom));background:var(--bg);color:var(--txt);min-height:100vh}
.wrap{max-width:480px;margin:0 auto}
.badge{display:inline-block;background:var(--pri);color:#fff;font-size:12px;font-weight:600;padding:4px 12px;border-radius:999px;margin-bottom:10px;letter-spacing:1px}
h1{font-size:21px;margin:0 0 4px}
.sub{font-size:13px;color:var(--mut);margin:0 0 14px;line-height:1.6}
.c{background:var(--card);border-radius:14px;padding:16px;margin:12px 0;box-shadow:0 1px 3px #0002;border:1px solid var(--line)}
.stat{display:flex;align-items:baseline;gap:8px}
.stat b{font-size:44px;font-weight:800;line-height:1;font-variant-numeric:tabular-nums}
.stat span{font-size:14px;color:var(--mut)}
.rows{margin-top:12px;font-size:14px;color:var(--mut);line-height:2}
.rows .k{display:inline-block;min-width:6em}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:1px}
.dot.on{background:var(--ok)}.dot.off{background:var(--err)}
button{width:100%;padding:15px;font-size:17px;font-weight:600;border:0;border-radius:12px;background:var(--pri);color:#fff;margin-top:8px;cursor:pointer;min-height:52px;transition:transform .06s,filter .15s;touch-action:manipulation}
button:active{transform:scale(.98);filter:brightness(.92)}
button:disabled{opacity:.45;cursor:not-allowed;transform:none}
input{width:100%;box-sizing:border-box;padding:14px;font-size:22px;text-align:center;border:1px solid var(--line);border-radius:10px;background:var(--bg);color:var(--txt);letter-spacing:8px;margin-top:8px}
input:focus{outline:2px solid var(--pri);outline-offset:-1px}
.big{font-size:44px;letter-spacing:10px;font-weight:800;text-align:center;margin:8px 0;color:var(--pri);font-variant-numeric:tabular-nums}
#m{font-size:15px;min-height:24px;margin-top:12px;line-height:1.7}
.wifibtn{display:block;text-align:center;padding:15px;background:var(--card);color:var(--txt);border:1px solid var(--line);border-radius:12px;text-decoration:none;font-size:15px;font-weight:600}
.hint{font-size:12px;color:var(--mut);text-align:center;margin:16px 0 8px;line-height:1.7}
</style></head><body><div class="wrap">
<span class="badge">本地備援模式</span>
<h1>&#9730;&#65039; 智能雨&#20438;寄存櫃</h1>
<p class="sub">此頁面供現場調試使用；日常存&#20438;／取&#20438;請透過線上網站（掃描櫃身 QR Code）</p>
<div class="c"><div class="stat"><b id="free">&#8211;</b><span>個空位</span></div>
<div class="rows">
<div><span class="k">取&#20438;口對正</span>第 <span id="cur">&#8211;</span> 格</div>
<div><span class="k">中繼伺服器</span><span id="srv"><span class="dot off"></span>檢查中&#8230;</span></div>
<div><span class="k">櫃子狀態</span><span id="busy">待機</span></div>
</div></div>
<div class="c"><b>存&#20438;</b><button id="bs" onclick="store()">我要存&#20438;</button></div>
<div class="c"><b>取&#20438;</b><input id="cd" inputmode="numeric" maxlength="4" placeholder="0000" autocomplete="off">
<button id="bt" onclick="take()">我要取&#20438;</button><div id="m"></div></div>
<a class="wifibtn" href="/wifi">&#9881;&#65039; WiFi 設定（更換網絡）</a>
<p class="hint">取&#20438;碼只會顯示一次，請截圖或抄下<br>櫃子聯網後即可使用，無需連接設備熱點</p>
</div>
<script>
const $=id=>document.getElementById(id);
let saved='';try{saved=localStorage.getItem('umbrellaCode')||''}catch(e){}
$('cd').value=saved;
async function j(u){const r=await fetch(u);return r.json()}
function msg(t,cls){$('m').innerHTML=t;$('m').style.color=cls==='err'?'var(--err)':(cls==='ok'?'var(--ok)':'')}
async function refresh(){try{const s=await j('/api/status');
 $('free').textContent=s.free;
 $('cur').textContent=s.cur||'\u2013';
 $('busy').textContent=s.busy?'\u26a1 運作中\u2026':'待機';
 $('srv').innerHTML='<span class="dot '+(s.server?'on':'off')+'"></span>'+(s.server?'在線（網站可控制）':'離線（僅本地可用）');
 $('bs').disabled=$('bt').disabled=s.busy;return s}
 catch(e){$('srv').innerHTML='<span class="dot off"></span>連線中斷';return null}}
async function wait(){for(let i=0;i<60;i++){const s=await refresh();if(s&&!s.busy)return;
 await new Promise(r=>setTimeout(r,1000))}}
async function store(){msg('請稍候\u2026');const r=await j('/api/store');
 if(!r.ok){msg(r.msg,'err');return}
 try{localStorage.setItem('umbrellaCode',r.code)}catch(e){}
 $('cd').value=r.code;
 msg('第 '+r.slot+' 格開門中，請放入雨&#20438;<br>你的取&#20438;碼<div class="big">'+r.code+'</div>請截圖或抄下');
 await wait();msg('&#9989; 完成，門已關閉','ok')}
async function take(){const c=$('cd').value.trim();if(c.length!==4){msg('請輸入 4 位取&#20438;碼','err');return}
 msg('請稍候\u2026');
 const r=await j('/api/take?code='+encodeURIComponent(c));
 if(!r.ok){msg(r.msg,'err');return}
 try{localStorage.removeItem('umbrellaCode')}catch(e){}
 msg('第 '+r.slot+' 格開門中，請取走雨&#20438;');
 await wait();msg('&#9989; 完成，歡迎再用','ok');$('cd').value=''}
refresh();setInterval(refresh,4000);
</script></body></html>
)rawliteral";

/* ===== 操作指示燈（非阻塞閃爍，不影響馬達與網頁回應）===== */
/* 信號設計（一顆燈區分存僘／取僘，持續到完成）：
   存僘中  慢閃：亮 0.3s / 滅 0.3s（轉動馬達期間）
   取僘中  快閃：亮 0.1s / 滅 0.1s
   開門中  常亮（門已開，請放僘／請取僘）
   完成    三連快閃（每次約 0.2s）後熄滅，回到待機
   待機    全部正常：每 3 秒輕閃一下
           中繼離線：每 3 秒雙閃（WiFi 有連但網站控制不通）
           WiFi 離線：每 1 秒快閃警示                                  */
int ledMode = 5;               // 0=滅 1=存僘中 2=取僘中 3=開門等待 4=完成(一次性) 5=待機
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
    case 1: on = (now % 600) < 300; break;   // 存僘：慢閃
    case 2: on = (now % 200) < 100; break;   // 取僘：快閃
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
      wifiFail = 0;                     // 連線成功，重置退避計數
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
  strncpy((char*)conf.sta.ssid, cfgSsid.length() ? cfgSsid.c_str() : WIFI_SSID, sizeof(conf.sta.ssid) - 1);
  strncpy((char*)conf.sta.password, cfgPass.length() ? cfgPass.c_str() : WIFI_PASS, sizeof(conf.sta.password) - 1);
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
  WiFi.setAutoReconnect(false);   // 關閉內部猛連（AP 拒絕時會形成 201/36 風暴），改用自己的退避重連
  /* 啟動時只試一輪（WPA2 約 8s → WPA3 約 6s，合計約 15 秒）：
     絕不在啟動時猛連——多輪猛連會觸發校園 AP 防護拉黑（原因碼 201/36 風暴）。
     之後由 loop 的 wifiTick 按 5s→15s→30s→60s 退避重連，給 AP 恢復時間。 */
  /* 主路徑：Arduino 默認 begin()——底層自動協商 WPA2/WPA3（含 WPA3-SAE），
     實測可直連升級成 WPA3 的校園 FCT_GUEST；自訂 esp_wifi_set_config 反而會協商失敗 */
  WiFi.begin(cfgSsid.length() ? cfgSsid.c_str() : WIFI_SSID,
             cfgPass.length() ? cfgPass.c_str() : WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) { delay(300); ledTick(); Serial.print("."); }
  if (WiFi.status() != WL_CONNECTED) {        // 保底：自訂 WPA3-SAE(H2E) 路徑
    Serial.println();
    Serial.print("  改用自訂 WPA3-SAE(H2E) 路徑再試一次 ");
    WiFi.disconnect(false); delay(300);
    wifiConnectWith(WIFI_AUTH_WPA3_PSK, WPA3_SAE_PWE_HASH_TO_ELEMENT, true);
    t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 6000) { delay(300); ledTick(); Serial.print("."); }
  }
  Serial.println();
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
  String tail = "";
  if (slot > 0) tail += " 第" + String(slot) + "格";
  if (code != "") tail += " 碼:" + code;
  Serial.printf("[WS] \u25ba 回覆：%s %s%s\n", ok ? "\u2705" : "\u274c", msg.c_str(), tail.c_str());
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
  if (force || (j != lastStatusJson && millis() - lastStatusPush > 300) || millis() - lastStatusPush > 1500) {
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
    /* 連續失敗自癒：約 100 秒連不上→重開機（重啟後走完整 setup 連接流程。
       注意：不可用 WiFi.reconnect()——本固件從不用 WiFi.begin，
       自定義配置只寫在 esp_wifi_set_config，reconnect() 會拿空/舊配置導致底層異常） */
    wsFailStreak++;
    if (wsFailStreak >= 20) {
      Serial.println("[WS] 連續 20 次連接失敗，重開機恢復");
      delay(100);
      ESP.restart();
    }
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
  Serial.printf("[WS] \u25c4 收到指令：%s\n", m.c_str());   // 即時監控：ESP32 實際收到的命令
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
        ledSetMode(1);                       // 指示燈：存僘慢閃，直到完成
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
      if (k < 0) pushResult(false, "取僘碼錯誤");
      else { jobType = 2; jobSlot = k; ledSetMode(2); pushStatus(true); }   // 指示燈：取僘快閃
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

/* 自主退避重連：失敗越多間隔越長（5s→15s→30s→60s 封頂），
   每 4 次切一次 WPA3——從根上避免猛連觸發 AP 防護拉黑（原因碼 201/36 風暴） */
void wifiTick() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  static const int waitSeq[] = { 5000, 15000, 30000, 60000 };
  int waitMs = waitSeq[wifiFail < 4 ? wifiFail : 3];
  if (now - lastWifiTry < (unsigned long)waitMs) return;
  lastWifiTry = now;
  wifiFail++;
  WiFi.disconnect(false);            // 清理殘留連接狀態（避免 sta is connecting 報錯）
  delay(50);
  if (wifiFail % 8 == 0) {
    /* 連續失敗時，偶爾回落自訂 WPA3 路徑保底（2026-10-22 實測：FCT_GUEST 升 WPA3 後
       默認 begin 可直連，自訂 esp_wifi_set_config 反而協商失敗） */
    Serial.printf("[WiFi] 第 %d 次重連（自訂 WPA3 保底路徑，退避 %dms）\n", wifiFail, waitMs);
    wifiConnectWith(WIFI_AUTH_WPA3_PSK, WPA3_SAE_PWE_HASH_TO_ELEMENT, true);
  } else {
    Serial.printf("[WiFi] 第 %d 次重連（默認路徑，退避 %dms）\n", wifiFail, waitMs);
    WiFi.begin(cfgSsid.length() ? cfgSsid.c_str() : WIFI_SSID,
               cfgPass.length() ? cfgPass.c_str() : WIFI_PASS);
  }
}

/* ===== 資料儲存（Preferences，斷電不遺失）===== */
void saveSlot(int i) {
  char k[6]; snprintf(k, sizeof(k), "s%d", i);
  if (slotCode[i] == "") prefs.remove(k); else prefs.putString(k, slotCode[i]);
}
void saveCur() { prefs.putInt("cur", curSlot); }
void loadAll() {
  curSlot = prefs.getInt("cur", 0);
  cfgSsid = prefs.getString("wifiSsid", "");   // 配網保存的 WiFi
  cfgPass = prefs.getString("wifiPass", "");
  for (int i = 0; i < SLOTS; i++) { char k[6]; snprintf(k, sizeof(k), "s%d", i); slotCode[i] = prefs.getString(k, ""); }
}
int freeCount() { int n = 0; for (int i = 0; i < SLOTS; i++) if (slotCode[i] == "") n++; return n; }
/* 讀取自訂 STA MAC（6 字節；沒有則全 0 = 用原廠） */
void loadStaMac() {
  if (prefs.getBytesLength("staMac") == 6) prefs.getBytes("staMac", staMac, 6);
}

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

/* ===== 存/取僘流程（在 loop 中執行）===== */
void runJob() {
  busy = true;
  int t = jobType, s = jobSlot; jobType = 0;
  pushStatus(true);
  rotateTo(s);
  setDoor(true);
  ledSetMode(3);                 // 開門等待：常亮（請放僘／請取僘）
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
  sendJson(String("{\"free\":") + freeCount() + ",\"total\":" + SLOTS +
           ",\"busy\":" + ((busy || jobType) ? "true" : "false") +
           ",\"cur\":" + (curSlot + 1) +
           ",\"server\":" + (serverOnline ? "true" : "false") + "}");
}
void apiStore() {
  if (busy || jobType) { sendJson("{\"ok\":false,\"msg\":\"櫃子運作中，請稍後再試\"}"); return; }
  int k = -1;
  for (int i = 0; i < SLOTS; i++) if (slotCode[i] == "") { k = i; break; }
  if (k < 0) { sendJson("{\"ok\":false,\"msg\":\"櫃子已滿\"}"); return; }
  String code = newCode();
  slotCode[k] = code; jobType = 1; jobSlot = k;
  ledSetMode(1);                       // 指示燈：存僘慢閃（本地備援路徑）
  sendJson(String("{\"ok\":true,\"slot\":") + (k + 1) + ",\"code\":\"" + code + "\"}");
}
void apiTake() {
  if (busy || jobType) { sendJson("{\"ok\":false,\"msg\":\"櫃子運作中，請稍後再試\"}"); return; }
  String code = server.arg("code"); code.trim();
  int k = -1;
  if (code != "") for (int i = 0; i < SLOTS; i++) if (slotCode[i] == code) { k = i; break; }
  if (k < 0) { sendJson("{\"ok\":false,\"msg\":\"取僘碼錯誤\"}"); return; }
  jobType = 2; jobSlot = k;
  ledSetMode(2);                       // 指示燈：取僘快閃（本地備援路徑）
  sendJson(String("{\"ok\":true,\"slot\":") + (k + 1) + "}");
}

/* ===== 本地管理頁（備援）===== */
void handleAdmin() {
  if (server.arg("key") != ADMIN_KEY) { server.send(403, "text/plain", "forbidden"); return; }
  String act = server.arg("act"), msg = "";
  int n = server.arg("n").toInt();
  noSvc = true;
  if (act != "" && (busy || jobType)) msg = "櫃子運作中";
  else if (act == "home" && n >= 1 && n <= SLOTS) { curSlot = n - 1; saveCur(); msg = "已對位：第 " + String(n) + " 格在取僘口"; }
  else if (act == "go" && n >= 1 && n <= SLOTS) { rotateTo(n - 1); msg = "已轉到第 " + String(n) + " 格"; }
  else if (act == "clear") { for (int i = 0; i < SLOTS; i++) { slotCode[i] = ""; saveSlot(i); } msg = "已清空全部格位"; }
  else if (act == "open") { setDoor(true); msg = "門已開"; }
  else if (act == "close") { setDoor(false); msg = "門已關"; }
  noSvc = false;
  String base = "/admin?key=" + String(ADMIN_KEY) + "&act=";
  String h = "<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:sans-serif;padding:14px'><h2>管理頁（本地）</h2><p>" + msg + "</p><p>連線：" + ipStr() + "</p><p>取僘口對正：第 " + String(curSlot + 1) + " 格</p><table border=1 cellpadding=6>";
  for (int i = 0; i < SLOTS; i++) { String cell = slotCode[i] == "" ? String("空") : slotCode[i]; h += "<tr><td>第 " + String(i + 1) + " 格</td><td>" + cell + "</td></tr>"; }
  h += "</table><p><a href='" + base + "open'>開門</a> | <a href='" + base + "close'>關門</a> | <a href='" + base + "clear'>清空全部</a></p><p>對位：";
  for (int i = 1; i <= SLOTS; i++) h += "<a href='" + base + "home&n=" + String(i) + "'>" + String(i) + "</a> ";
  h += "</p><p>轉到：";
  for (int i = 1; i <= SLOTS; i++) h += "<a href='" + base + "go&n=" + String(i) + "'>" + String(i) + "</a> ";
  h += "</p></body>";
  server.send(200, "text/html; charset=utf-8", h);
}

/* ===== WiFi 配網頁（離線調整連接的網絡）=====
   手機連 Umbrella 熱點 → http://192.168.4.1/wifi 即可設定 */
/* URL 百分號編解碼（中文 SSID 必備） */
String urlEncode(const String& s) {
  String r; r.reserve(s.length() * 3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (isalnum(c) || strchr("-_.~", c)) r += (char)c;
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}
String urlDecode(const String& s) {
  String r; r.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') r += ' ';
    else if (c == '%' && i + 2 < s.length()) {
      auto hx = [](char x) -> int {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'a' && x <= 'f') return x - 'a' + 10;
        if (x >= 'A' && x <= 'F') return x - 'A' + 10;
        return -1;
      };
      int v1 = hx(s[i + 1]), v2 = hx(s[i + 2]);
      if (v1 >= 0 && v2 >= 0) { r += (char)((v1 << 4) | v2); i += 2; }
      else r += c;
    } else r += c;
  }
  return r;
}

void wifiPageHead(const String& body) {
  server.send(200, "text/html; charset=utf-8",
    String("<!doctype html><html lang=\"zh-Hant\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
    "<title>WiFi &#35731;&#23450;</title><style>"
    ":root{--bg:#f4f6f8;--card:#fff;--txt:#1b2733;--mut:#5a6b7b;--pri:#1f6feb;--ok:#1d7a1d;--err:#b3261e;--line:#dde3ea}"
    "@media(prefers-color-scheme:dark){:root{--bg:#0f151c;--card:#1a232e;--txt:#e8eef4;--mut:#93a3b3;--pri:#4d8ef7;--ok:#6bd06b;--err:#ff9e94;--line:#2c3a48}}"
    "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}"
    "body{font-family:-apple-system,BlinkMacSystemFont,\"Ping Fang TC\",\"Noto Sans TC\",sans-serif;margin:0;padding:calc(14px+env(safe-area-inset-top)) 16px calc(16px+env(safe-area-inset-bottom));background:var(--bg);color:var(--txt);min-height:100vh}"
    ".wrap{max-width:480px;margin:0 auto}h1{font-size:21px;margin:0 0 4px}.sub{font-size:13px;color:var(--mut);margin:0 0 14px;line-height:1.6}"
    ".c{background:var(--card);border-radius:14px;padding:16px;margin:12px 0;box-shadow:0 1px 3px #0002;border:1px solid var(--line)}"
    ".st{font-size:14px;line-height:1.9}.st b{color:var(--txt)}"
    ".dim{color:var(--mut);font-size:13px}"
    "h3{font-size:15px;margin:20px 0 10px;display:flex;align-items:center;gap:8px}"
    "h3 .no{background:var(--pri);color:#fff;width:22px;height:22px;border-radius:50%;display:inline-flex;align-items:center;justify-content:center;font-size:13px;font-weight:700;flex-shrink:0}"
    ".net{display:flex;align-items:center;gap:10px;width:100%;padding:13px 14px;font-size:15px;border:1px solid var(--line);border-radius:12px;background:var(--card);color:var(--txt);margin:8px 0;cursor:pointer;text-align:left;min-height:52px;touch-action:manipulation}"
    ".net:active{filter:brightness(.94)}.net.sel{border-color:var(--pri);border-width:2px;padding:12px 13px}"
    ".net .nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:600}"
    ".net .sg{display:inline-flex;align-items:flex-end;gap:2px;height:14px}"
    ".net .sg i{width:4px;border-radius:1px;background:var(--mut);display:block}"
    ".net .sg i.a{background:var(--ok)}"
    ".net .db{font-size:12px;color:var(--mut);font-variant-numeric:tabular-nums;min-width:64px;text-align:right}"
    ".lk{font-size:13px}"
    "input{width:100%;box-sizing:border-box;padding:14px;font-size:17px;border:1px solid var(--line);border-radius:10px;background:var(--bg);color:var(--txt);margin:8px 0}"
    "input:focus{outline:2px solid var(--pri);outline-offset:-1px}"
    ".pw{position:relative}.pw input{padding-right:64px}.pw button{position:absolute;right:6px;top:14px;width:auto;min-height:0;padding:8px 10px;font-size:13px;background:transparent;color:var(--pri);border:0;margin:0;font-weight:600}"
    "button.go{width:100%;padding:15px;font-size:17px;font-weight:700;border:0;border-radius:12px;background:var(--pri);color:#fff;margin-top:14px;cursor:pointer;min-height:52px;touch-action:manipulation}"
    "button.go:active{filter:brightness(.92)}button.go:disabled{opacity:.5;cursor:not-allowed}"
    ".adv{margin-top:22px;padding-top:16px;border-top:1px solid var(--line);font-size:13px;color:var(--mut);line-height:2}"
    ".adv a{color:var(--pri)}"
    ".foot{font-size:12px;color:var(--mut);text-align:center;margin:18px 0 8px;line-height:1.7}"
    "</style></head><body><div class=\"wrap\">") + body + "</div></body></html>");
}

/* WiFi 掃描 API（異步，配網頁 JS 呼叫，避免阻塞頁面渲染） */
void wifiScanApi() {
  noSvc = true;
  int n = WiFi.scanNetworks(false, false, false, 300);
  struct Net { String s; int r; bool l; };
  Net nets[20];
  int cnt = 0;
  for (int i = 0; i < n && cnt < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    nets[cnt].s = s; nets[cnt].r = WiFi.RSSI(i);
    nets[cnt].l = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    cnt++;
  }
  WiFi.scanDelete();
  /* 按訊號強度排序（強 → 弱） */
  for (int a = 0; a < cnt - 1; a++)
    for (int b = 0; b + 1 < cnt - a; b++)
      if (nets[b].r < nets[b + 1].r) { Net x = nets[b]; nets[b] = nets[b + 1]; nets[b + 1] = x; }
  String j = "{\"nets\":[";
  for (int i = 0; i < cnt; i++) {
    if (i) j += ",";
    j += "{\"ssid\":\"" + urlEncode(nets[i].s) + "\",\"rssi\":" + String(nets[i].r) +
         ",\"lock\":" + (nets[i].l ? "true" : "false") + "}";
  }
  j += "]}";
  noSvc = false;
  sendJson(j);
}

/* ===== WiFi 配網頁（展覽／異地免燒錄更換網絡）=====
   手機連 Umbrella 熱點 → http://192.168.4.1/wifi */
void handleWifi() {
  noSvc = true;
  String st = WiFi.status() == WL_CONNECTED
    ? String("<div class=\"c st\">&#128279; <b>已連線</b>：" + WiFi.SSID() + "<br>&#127760; " + ipStr() +
             (serverOnline ? "<br>&#9724;&#65039; 中繼伺服器在線（網站可控制）" : "<br>&#9888;&#65039; 中繼伺服器離線（僅本地可用）") + "</div>")
    : String("<div class=\"c st\" style=\"border-color:var(--err)\">&#9888;&#65039; <b>目前未連線</b><br>請在下面選擇網絡並輸入密碼；設定前請保持手機連著 <b>Umbrella</b> 熱點</div>");
  String src = cfgSsid.length() ? cfgSsid : String("（尚未設定，用寫死預設）");
  noSvc = false;
  String h = String("<h1>&#9881;&#65039; WiFi &#35731;&#23450;</h1><p class=\"sub\">更換櫃子連接的 WiFi &#8212; 儲存後自動重啟並連接新網絡</p>") + st +
    "<h3><span class=\"no\">1</span>選擇網絡<span class=\"dim\" id=\"scanning\">（掃描中&#8230;）</span></h3><div id=\"nets\"></div>"
    "<h3><span class=\"no\">2</span>輸入密碼</h3>"
    "<form id=\"f\" action='/wifi/save' method='post' onsubmit=\"return submitForm()\">"
    "<input id='ssid' name='ssid' placeholder='WiFi &#21517;&#31281;&#65288;SSID&#65289;' autocomplete='off' autocapitalize='off'>"
    "<div class='pw'><input id='pass' name='pass' type='password' placeholder='WiFi &#23494;&#30908;' autocomplete='off'>"
    "<button type='button' id='eye'>顯示</button></div>"
    "<button class='go' type='submit' id='go'>&#23384;&#20729;&#20006;&#37325;&#26032;&#21551;&#21205;</button></form>"
    "<div class='adv'>目前使用：" + src + "<br>"
    "設定掉電不丟；連錯網絡可重連 Umbrella 熱點再來一次<br>"
    "<a href='/wifi/reset'>&#24674;&#22797;&#20889;&#27515;&#38928;&#35330; WiFi</a><br>"
    "<a href='/wifi/mac'>&#26356;&#25463; MAC&#65288;AP &#21049;&#40657;&#26178;&#29992;&#65289;</a>——重連很久仍失敗時點一下，換個網路身份重來</div>"
    "<p class='foot'>支援中文 WiFi &#21517;&#31281; &#183; 密碼上限 63 &#20301;&#20803;</p>"
    "<script>"
    "const $=id=>document.getElementById(id);"
    "function bars(r){const n=r>-55?4:(r>-67?3:(r>-78?2:1));let h='';for(let i=1;i<=4;i++)h+='<i class=\"'+(i<=n?'a':'')+'\" style=\"height:'+(i*25)+'%\"></i>';return '<span class=\"sg\">'+h+'</span>'}"
    "fetch('/wifi/scan').then(r=>r.json()).then(d=>{"
    "$('scanning').textContent='';"
    "const box=$('nets');"
    "if(!d.nets||!d.nets.length){box.innerHTML='<p class=\"dim\">未發現網絡，請靠近路由器後重新整理頁面</p>';return}"
    "box.innerHTML=d.nets.map(n=>{const s=decodeURIComponent(n.ssid);"
    "return '<button type=\"button\" class=\"net\" data-s=\"'+s.replace(/\"/g,'&quot;')+'\">'+"
    "'<span class=\"nm\">'+s+'</span>'+bars(n.rssi)+'<span class=\"db\">'+n.rssi+' dBm</span>'+(n.lock?' <span class=\"lk\">&#128274;</span>':'')+'</button>'}).join('');"
    "box.querySelectorAll('.net').forEach(b=>b.onclick=()=>{"
    "$('ssid').value=b.dataset.s;$('pass').value='';$('pass').focus();"
    "box.querySelectorAll('.net').forEach(x=>x.classList.remove('sel'));b.classList.add('sel');});"
    "}).catch(()=>{$('scanning').textContent='(掃描失敗，請重新整理)'});"
    "$('eye').onclick=()=>{const p=$('pass');p.type=p.type==='password'?'text':'password';$('eye').textContent=p.type==='password'?'顯示':'隱藏'};"
    "function submitForm(){const s=$('ssid').value.trim();"
    "if(!s){alert('請先點選上方網絡，或手動輸入 WiFi 名稱');return false}"
    "$('go').disabled=true;$('go').textContent='儲存中…';return true}"
    "</script>";
  wifiPageHead(h);
}

void handleWifiSave() {
  noSvc = true;
  String s = urlDecode(server.arg("ssid")); s.trim();
  String pw = urlDecode(server.arg("pass"));
  if (s == "") {
    noSvc = false;
    wifiPageHead("<h2>SSID 不能為空</h2><p><a href='/wifi'>返回</a></p>");
    return;
  }
  if (pw.length() > 63) pw = pw.substring(0, 63);   // WPA2 密碼上限 63 位
  prefs.putString("wifiSsid", s);
  prefs.putString("wifiPass", pw);
  wifiPageHead(String("<h2>\u2705 已儲存</h2><p>WiFi：<b>") + s + "</b></p><p>3 秒後自動重新啟動並連接新網絡…</p><p style='color:#888;font-size:13px'>若連不上，櫃子會保留 Umbrella 熱點，重連熱點開 http://192.168.4.1/wifi 即可再設定</p>");
  delay(2500);
  ESP.restart();
}

/* 更換 STA MAC：被學校 AP 拉黑（重連風暴）時的一鍵解封 */
void handleWifiMac() {
  noSvc = true;
  uint8_t mac[6];
  mac[0] = 0x02;                                   // 本地管理位（避免與真實網卡衝突）
  for (int i = 1; i < 6; i++) mac[i] = (uint8_t)esp_random();
  prefs.putBytes("staMac", mac, 6);
  char buf[40];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  wifiPageHead(String("<h2>✅ 已更換 MAC</h2><p>新 MAC：<b>") + buf +
               "</b></p><p>3 秒後重新啟動，將以新身份連接網絡（可繞過 AP 對舊 MAC 的封鎖）。</p>"
               "<p style='color:#888;font-size:13px'>若仍連不上，請確認 WiFi 密碼是否正確，或稍等片刻再試。</p>");
  delay(2500);
  ESP.restart();
}

void handleWifiReset() {
  noSvc = true;
  prefs.remove("wifiSsid");
  prefs.remove("wifiPass");
  wifiPageHead("<h2>\u2705 已恢復默認</h2><p>3 秒後重新啟動，將連接寫死預設的 WiFi。</p>");
  delay(2500);
  ESP.restart();
}

/* ===== 序列埠指令（除錯用，速率 115200）===== */
const char* ledName() {
  if (ledBlinks > 0) return "完成閃爍";
  switch (ledMode) {
    case 1: return "存僘慢閃";
    case 2: return "取僘快閃";
    case 3: return "開門常亮";
    case 5: return "待機";
    default: return "熄滅";
  }
}

void handleSerial() {
  String c = Serial.readStringUntil('\n'); c.trim();
  if (c == "status") {
    Serial.println("連線：" + ipStr() + (serverOnline ? " ＋ 中繼伺服器在線" : "（中繼離線）"));
    Serial.printf("取僘口對正：第 %d 格，空格 %d/%d\n", curSlot + 1, freeCount(), SLOTS);
    Serial.printf("指示燈：%s\n", ledName());
    for (int i = 0; i < SLOTS; i++) Serial.printf("  第 %d 格：%s\n", i + 1, slotCode[i] == "" ? "(空)" : slotCode[i].c_str());
  }
  else if (c == "trydefault") {   // 測試 Arduino 預設 begin() 是否可連 WPA3 網絡
    Serial.println("用 Arduino 預設 WiFi.begin 嘗試連接（15 秒）...");
    WiFi.disconnect(false); delay(200);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) { delay(300); Serial.print("."); }
    Serial.println();
    Serial.println(WiFi.status() == WL_CONNECTED ? "預設路徑連上了" : "預設路徑也失敗");
  }
  else if (c == "newmac") {        // 一鍵更換 STA MAC（學校按設備類型/OUI 封鎖 ESP32 時用）
    uint8_t mac[6];
    mac[0] = 0x02;
    for (int i = 1; i < 6; i++) mac[i] = (uint8_t)esp_random();
    prefs.putBytes("staMac", mac, 6);
    Serial.printf("新 MAC：%02X:%02X:%02X:%02X:%02X:%02X，3 秒後重啟\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    delay(2500);
    ESP.restart();
  }
  else if (c == "netinfo") {        // 網路診斷：顯示實際保存的 WiFi 憑證與 MAC
    Serial.println("=== 網路診斷 ===");
    Serial.println("保存的 SSID: [" + cfgSsid + "]");
    Serial.println("保存的密碼: [" + cfgPass + "]");
    Serial.println("WiFi 狀態: " + String(WiFi.status() == WL_CONNECTED ? "已連線" : "未連線(" + String(WiFi.status()) + ")"));
    if (WiFi.status() == WL_CONNECTED) Serial.println("連到: " + WiFi.SSID() + " (" + ipStr() + ")");
    uint8_t m2[6]; esp_wifi_get_mac(WIFI_IF_STA, m2);
    Serial.printf("STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", m2[0], m2[1], m2[2], m2[3], m2[4], m2[5]);
    Serial.printf("連續失敗: WiFi %d 次 / WS %d 次\n", wifiFail, wsFailStreak);
  }
  else if (c == "led test") {           // 指示燈測試：依序示範全部信號（約 7 秒）
    Serial.println("指示燈測試 → 存僘慢閃");   ledSetMode(1); waitMs(2000);
    Serial.println("指示燈測試 → 取僘快閃");   ledSetMode(2); waitMs(2000);
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
  Serial.println("\n===== 智能旋轉式雨僘寄存櫃 啟動（在線控制版 v4.2）=====");
  WiFi.onEvent(onWiFiEvent);                 // WiFi 事件日誌（連線診斷）
  pinMode(STEP_PIN, OUTPUT); pinMode(DIR_PIN, OUTPUT); pinMode(EN_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);   // 指示燈預設熄滅
  digitalWrite(EN_PIN, HIGH);                      // 開機先斷電
  ledSetMode(5);                                   // 待機信號：在線每 3 秒輕閃
  servoA.setPeriodHertz(50); servoB.setPeriodHertz(50);
  servoA.attach(SERVO_A_PIN, 500, 2500); servoB.attach(SERVO_B_PIN, 500, 2500);
  prefs.begin("umbrella", false); loadAll(); loadStaMac();
  setDoor(false);

#if ALWAYS_AP
  WiFi.mode(WIFI_AP_STA);                          // 本地熱點 + 校園 WiFi 同時在線
  /* 被 AP 拉黑（原因碼 201/36/4 風暴）時，配網頁「更換 MAC」會存一組新 MAC 到這裡 */
  if (staMac[0] || staMac[1] || staMac[2] || staMac[3] || staMac[4] || staMac[5]) {
    esp_wifi_set_mac(WIFI_IF_STA, staMac);
    Serial.printf("使用自訂 STA MAC：%02X:%02X:%02X:%02X:%02X:%02X\n",
                  staMac[0], staMac[1], staMac[2], staMac[3], staMac[4], staMac[5]);
  }
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
    Serial.printf("請用手機連 %s 熱點，開 http://192.168.4.1/wifi 設定 WiFi\n", AP_SSID);
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
  server.on("/wifi", handleWifi);
  server.on("/wifi/scan", wifiScanApi);
  server.on("/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/wifi/reset", handleWifiReset);
  server.on("/wifi/mac", handleWifiMac);
  server.onNotFound([]() { if (apMode) { server.sendHeader("Location", "http://192.168.4.1/", true); server.send(302, "text/plain", ""); } else server.send(404, "text/plain", "Not found"); });
  server.begin(); started = true;

  /* 中繼伺服器（在線控制） */
  String srv = String(SERVER_URL);
  serverConfigured = (srv.length() > 0 && srv.indexOf("your-server") < 0);
  if (serverConfigured) {
    Serial.println("中繼伺服器：" + srv);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(1500);   // 斷線後 1.5 秒即重試（原 5 秒太久）
    wsTick();
  } else {
    Serial.println("尚未設定中繼伺服器網址（SERVER_URL），在線控制未啟用；本地備援正常");
  }
  Serial.printf("操作指示燈：GPIO%d（存僘慢閃／取僘快閃／開門常亮／完成三連閃；序列埠 led test 可測試）\n", LED_PIN);
  Serial.println("注意：本版無歸零感測器，轉盤被手動轉動後請用 home <格號> 重新對位");
}

void loop() {
  service();
  wsTick();
  if (jobType) runJob();
  if (serverOnline) pushStatus(false);
  if (Serial.available()) handleSerial();
  wifiTick();   // 退避式 WiFi 重連（替代原來的 15 秒硬重連）
  delay(2);
}
