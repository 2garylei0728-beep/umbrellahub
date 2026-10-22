# ESP32 草稿码燒錄說明（v4.2 在線控制版）

## 需要安裝的程式庫（Arduino IDE → 程式庫管理員）

| 搜尋 | 安裝 |
|---|---|
| `ESP32Servo` | ESP32Servo by Kevin Harrington（**不要**裝只有 Servo 的那個） |
| `WebSockets` | WebSockets by Markus Sattler (Links2004) |

開發板套件（Board 選 `ESP32 Dev Module`，Upload Speed 115200）：

- **esp32 by Espressif Systems 3.x（本專案實測 3.3.12 / IDF 5.5）**——校園 FCT_GUEST 已升級 WPA3-H2E，IDF 5.1 起才完整支援，**連校園網必須用 3.x**；
- 教學指定的 **2.0.17（IDF 4.4）照樣可用**，但連不上校園 FCT_GUEST（認證失敗 202，框架限制），家用 WiFi／手機熱點正常。
- 兩套工具鏈互相獨立，可同時安裝（2.0.17 留在 Arduino IDE 預設位置，3.x 另裝），互不影響。

## 中繼伺服器網址（已填入，2026-10-20 上線）

第 38 行 `SERVER_URL` **已填入 Manus 正式網址並燒錄驗證通過**：

```cpp
const char* SERVER_URL = "wss://umbrellahub-caozd3vf.manus.space/ws?role=device&secret=T3pVK7eNVgl_hJEuZCE4XxdL1c4AkXM6";
```

- 網站網址（QR 码內容）：`https://umbrellahub-caozd3vf.manus.space`
- 換平台（例如改用 `hf-space/` 的 Hugging Face 版）時，只需改這一個字串再重燒錄。
- 若日後更換 `DEVICE_SECRET`，兩邊（Manus 環境變數 + 這裡的 `secret=`）必須同步改。

未設定（含 `your-server` 佔位符）時，ESP32 只跑本地備援，不會連中繼。

## 燒錄後驗證（序列埠監控器 115200）

```
===== 智能旋轉式雨傘寄存櫃 啟動（在線控制版 v4.2）=====
本地備援熱點：Umbrella （192.168.4.1）
連線到校園 WiFi：FCT_GUEST ...
[WiFi] 已連上 AP（FCT_GUEST）
成功！IP：192.168.128.248（備用 http://umbrella.local）
中繼伺服器：wss://umbrellahub-caozd3vf.manus.space/ws?role=device&secret=... ...
[WS] 已連接中繼伺服器，在線控制就緒
操作指示燈：GPIO4（存傘慢閃／取傘快閃／開門常亮／完成三連閃；序列埠 led test 可測試）
```

看到「在線控制就緒」後，用手機開 `https://umbrellahub-caozd3vf.manus.space` 即可控制。
（以上為 2026-10-20 實測輸出；Manus 為 Autoscale 模式，閒置後首次開網站有數秒冷啟動，ESP32 約 5 秒自動重連，稍候即恢復。）

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

固件用底層 `esp_wifi_set_config` 設定認證（Arduino 的 `WiFi.begin` 只支援 WPA2）：

1. **第一嘗試 = WPA2-PSK**（與 Arduino 預設相同）：家用路由器、手機熱點、學校舊網絡都走這條，通常 1～2 秒連上。
2. **第二嘗試 = WPA3-SAE（H2E 強制 + PMF）**：給純 WPA3 網絡。
3. 每輪失敗會換 AP 重試，最多 4 輪（最壞約 60 秒），之後由主迴圈每 15 秒持續重試。

**關於校園 FCT_GUEST（已解決 ✅）**：學校網絡已升級為 WPA3-個人（H2E 強制）。排查結論：

- 密碼有效（電腦可連）；
- esp32 套件 **2.0.17（IDF 4.4）的 WPA3-H2E 支援不完整**，認證被拒（原因碼 202 = AUTH_FAIL），屬框架限制；
- 升級到 **3.x（IDF 5.1+，實測 3.3.12 / IDF 5.5）後直接連上**：開機約 1 秒取得 IP（實測 192.168.128.248），90 秒觀察零掉線。

修改 `WIFI_SSID` / `WIFI_PASS` 即可切換目標網絡。

### 3.x 工具鏈安裝記錄（arduino-cli，與 2.0.17 並存）

本專案 3.x 工具鏈用 arduino-cli 裝在獨立數據目錄，不動教學指定的 2.0.17：

```bash
# 1) 獨立配置（數據目錄 arduino15b、sketchbook-b，與舊的 arduino15 完全隔離）
#    arduino15b/arduino-cli.yaml：data=…/tools/arduino15b，user=…/tools/sketchbook-b

# 2) 安裝核心與程式庫
arduino-cli --config-file arduino15b/arduino-cli.yaml core update-index
arduino-cli --config-file arduino15b/arduino-cli.yaml core install esp32:esp32@3.3.12
arduino-cli --config-file arduino15b/arduino-cli.yaml lib install "ESP32Servo" "WebSockets"

# 3) 編譯＋燒錄（ESP32Servo 3.2.1、WebSockets 2.7.2，API 與舊版相同，草稿码無需改動）
arduino-cli --config-file arduino15b/arduino-cli.yaml compile \
  --fqbn esp32:esp32:esp32:UploadSpeed=115200 umbrella_cabinet_online
arduino-cli --config-file arduino15b/arduino-cli.yaml upload \
  --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port COM11 umbrella_cabinet_online
```

實測草稿码 1,181,054 位元組（90%），塞得進 3.x 預設分區（1.25MB），**不需要**改 `PartitionScheme`。

Arduino IDE 使用者則在「開發板管理員」裝 esp32 3.x 後，於「工具 → 開發板」選對應版本即可，程式庫版本同上表。

## 展覽／異地上網：免燒錄更換 WiFi（2026-10-21 已實測）

櫃子搬到沒有 FCT_GUEST 的場地時，**不用電腦、不用燒錄**，用手機 2 分鐘搞定：

1. 櫃子通電後連不上原 WiFi → 會持續重試（約 2 分鐘內會自動重啟再試）；
2. 手機 WiFi 設定裡連上熱點 **`Umbrella`**（密碼 `12345678`）；
3. 手機瀏覽器開 **`http://192.168.4.1/wifi`**；
4. 頁面會列出附近網絡（含訊號強度）——**點一下要連的網絡**（自動填入名稱），或手動輸入 SSID；
5. 填密碼 → 按「**儲存並重新啟動**」；
6. 櫃子約 30 秒後自動連上新 WiFi 並重連中繼伺服器（雲端 `/api/status` 顯示 `online:true` 即成功）。

其他要知道的：
- 本地首頁 `http://192.168.4.1/` 也有「⚙️ WiFi 設定（更換網絡）」按鈕，同一頁面；
- 連錯網絡／密碼填錯：重開熱點再進 `/wifi` 重新設定即可（設定存在 ESP32 快閃記憶體，斷電不丟）；
- 「恢復寫死預設 WiFi」連結會清掉自訂設定，回到草稿码裡寫死的 FCT_GUEST；
- 支援中文 SSID（已做 URL 編碼處理）；密碼上限 63 位元（WPA2 規範）；
- 設定頁不需要管理密碼——能連上 Umbrella 熱點代表人在櫃子旁邊，這本身就是物理存取控制。

## 本地備援與除錯（連線失敗時）

- 本地熱點 `Umbrella`（密碼 `12345678`）→ 瀏覽器 `http://192.168.4.1`
- 序列埠指令（115200）：
  - `status` 連線與各格狀態（含指示燈模式）
  - `scan` 掃描附近網絡（認證碼：0=開放、3=WPA2、4=WPA/WPA2 混合、6=WPA3、7=WPA2/WPA3 過渡）——排查網絡問題用
  - `led test` 指示燈信號示範
  - `open` / `close` / `step 3200` / `go 3` / `home 1` / `clear`
- 想關掉本地熱點（只保留在線控制）：把 `ALWAYS_AP` 改 `0`

## 接線（與前版相同）

TMC2209 STEP/DIR/EN = GPIO26/27/25；舵機 A/B = GPIO32/33；指示燈 = GPIO4（經 220Ω 電阻）。
MS1、MS2 接 3V3（1/16 細分）。USB 供電調試；接馬達與舵機後改降壓模組獨立 5V 供電，兩者不要同時接。
