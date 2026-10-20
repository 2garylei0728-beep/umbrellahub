# ESP32 草稿码燒錄說明（v4.2 在線控制版）

## 需要安裝的程式庫（Arduino IDE → 程式庫管理員）

| 搜尋 | 安裝 |
|---|---|
| `ESP32Servo` | ESP32Servo by Kevin Harrington（**不要**裝只有 Servo 的那個） |
| `WebSockets` | WebSockets by Markus Sattler (Links2004) |

開發板套件：esp32 by Espressif Systems **2.0.17**（Board 選 `ESP32 Dev Module`，Upload Speed 115200）。

## 燒錄前必改：中繼伺服器網址

部署好網站後，把第 29 行的 `SERVER_URL` 改成實際網址：

```cpp
const char* SERVER_URL = "wss://你的網址/ws?role=device&secret=你的DEVICE_SECRET";
```

- `secret` 必須與伺服器的 `DEVICE_SECRET` 環境變數完全相同。
- 若網址是 `https://` 開頭，這裡就用 `wss://`（例如 `https://umbrella-cabinet.onrender.com` → `wss://umbrella-cabinet.onrender.com/ws?...`）。

未設定（含 `your-server` 佔位符）時，ESP32 只跑本地備援，不會連中繼。

## 燒錄後驗證（序列埠監控器 115200）

```
===== 智能旋轉式雨傘寄存櫃 啟動（在線控制版 v4.2）=====
本地備援熱點：Umbrella （192.168.4.1）
連線到校園 WiFi：FCT_GUEST ...
成功！IP：192.168.x.x
[WS] 連接伺服器 your-host:443/ws?role=device&secret=... ...
[WS] 已連接中繼伺服器，在線控制就緒
操作指示燈：GPIO4（存傘慢閃／取傘快閃／開門常亮／完成三連閃；序列埠 led test 可測試）
```

看到「在線控制就緒」後，用手機開網站網址即可控制。

## 操作指示燈（v4.1 新增）

接線：**GPIO4 → 220Ω 電阻 → LED 長腳(+)；LED 短腳(-) → GND**。
備用腳位：5 / 16 / 17（PSRAM 板勿用 16/17）/ 18 / 19。若開發板內置燈（多為 GPIO2），把 `LED_PIN` 改 2 即可免接線。

| 狀態 | 信號 |
|---|---|
| 存傘中（馬達轉動） | 慢閃：亮 0.3s / 滅 0.3s |
| 取傘中（馬達轉動） | 快閃：亮 0.1s / 滅 0.1s |
| 開門等待放/取傘 | 常亮 |
| 完成 | 三連快閃（每次 0.2s）後熄滅 |
| 待機・全部正常 | 每 3 秒輕閃一下 |
| 待機・中繼離線 | 每 3 秒雙閃（WiFi 有連但網站控制不通） |
| 待機・WiFi 離線 | 每 1 秒快閃警示 |

信號由手機網站（或本地備援頁）的存/取傘指令觸發，持續到完成；閃爍完全非阻塞，不影響馬達與網頁回應。序列埠送 `led test` 可依序示範全部信號（約 7 秒）。

## WiFi 認證說明（v4.2 新增，重要）

固件現在用底層 `esp_wifi_set_config` 設定認證（Arduino 的 `WiFi.begin` 只支援 WPA2）：

1. **第一嘗試 = WPA2-PSK**（與 Arduino 預設相同）：家用路由器、手機熱點、學校舊網絡都走這條，通常 1～2 秒連上。
2. **第二嘗試 = WPA3-SAE（H2E 強制 + PMF）**：給純 WPA3 網絡。
3. 每輪失敗會換 AP 重試，最多 4 輪（最壞約 60 秒），之後由主迴圈每 15 秒持續重試。

**關於校園 FCT_GUEST**：學校網絡已升級為 **WPA3-個人（H2E 強制）**。實測確認：
- 密碼仍然有效（電腦可連上）；
- ESP32（Arduino 2.0.17 / IDF 4.4）的 WPA2-PSK 與 WPA3-SAE 認證**都被 AP 拒絕**（序列埠原因碼 202 = AUTH_FAIL）——IDF 4.4 的 WPA3-H2E 支援不完整，這是框架限制，非程式錯誤；
- FCT_GUEST 雖廣播 WPA2/WPA3 過渡模式（認證碼 7），但實際拒絕 WPA2-PSK 客戶端。

因此**展示/比賽時建議用手機熱點供網**（ESP32 連熱點 → 互聯網 → 中繼伺服器），或請學校資訊組開一個 WPA2 的 IoT 專用 SSID。想彻底支援校園 WPA3-H2E，需升級到 esp32 開發板套件 3.x（IDF 5.1 起 H2E 才完整）——注意 3.x 與教學指定的 2.0.17 是兩套工具鏈，需分開安裝。

修改 `WIFI_SSID` / `WIFI_PASS` 即可切換目標網絡。

## 本地備援與除錯（連線失敗時）

- 本地熱點 `Umbrella`（密碼 `12345678`）→ 瀏覽器 `http://192.168.4.1`
- 序列埠指令（115200）：
  - `status` 連線與各格狀態（含指示燈模式）
  - `scan` 掃描附近網絡（認證碼：3=WPA2、6=WPA3、7=WPA2/WPA3 過渡）——排查網絡問題用
  - `led test` 指示燈信號示範
  - `open` / `close` / `step 3200` / `go 3` / `home 1` / `clear`
- 想關掉本地熱點（只保留在線控制）：把 `ALWAYS_AP` 改 `0`

## 接線（與前版相同）

TMC2209 STEP/DIR/EN = GPIO26/27/25；舵機 A/B = GPIO32/33；指示燈 = GPIO4（經 220Ω 電阻）。
MS1、MS2 接 3V3（1/16 細分）。USB 供電調試；接馬達與舵機後改降壓模組獨立 5V 供電，兩者不要同時接。
