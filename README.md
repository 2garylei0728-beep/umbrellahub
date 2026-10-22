# 智能旋轉式雨傘寄存櫃 — 在線控制網站

手機／電腦**打開網址即可控制雨傘櫃**，不需要連設備的 WiFi。
ESP32 只需連上互聯網（家用 WiFi、手機熱點或校園 WiFi），透過 WebSocket 與本伺服器保持長連線。

## 架構

```
手機/電腦瀏覽器 ──HTTPS/WSS──► 本伺服器（Node.js）──WSS──► ESP32（連互聯網）
   用戶只開網址                     指令中繼＋狀態廣播            驅動轉盤/門片
```

## ⚠️ 供網方式提醒（2026-10-20 實測）

學校 FCT_GUEST 已升級 **WPA3-個人（H2E 強制）**，ESP32（Arduino 2.0.17 / IDF 4.4）無法完成認證（原因碼 202）。
**展示／比賽建議用手機熱點供網**（ESP32 連熱點 → 互聯網 → 本伺服器），或請學校資訊組提供 WPA2 的 IoT 專用 SSID。
固件已內建 WPA2-PSK / WPA3-SAE 雙認證與多輪重試，家用 WiFi 與熱點均可自動連上；詳見 `esp32-firmware/README.md`。

## 一、部署

這是一個標準 Node.js 專案（Express + ws），**需要支援 WebSocket 長連線的常駐 Node 環境**。

### ⭐ Manus（本專案的主要目標平台，詳見 `MANUS_DEPLOY.md`）
把整個專案（或本 ZIP）交給 Manus，讓它建立網站並發布。最佳設定是 Reserved（預留實例）；若帳戶只能使用 Autoscale，ESP32 仍會自動重連，但閒置後第一次操作可能遇到冷啟動。

### Render（備選，可用 render.yaml 一鍵部署）
1. 將本專案推到 Git 倉庫（或直接在 Render 後台連結）。
2. Render 會讀取 `render.yaml` 自動建立 Web Service。
3. 在後台設定環境變數（見下）。
4. 部署完成後得到網址，例如 `https://umbrella-cabinet.onrender.com`。

### Railway / Fly.io / Glitch / 自架伺服器
- Build Command：`npm install`
- Start Command：`npm start`
- 需開放對外 HTTPS（平台自動處理 TLS；應用本身只用普通 HTTP/WS）

> 不適用：純靜態平台（GitHub Pages / Vercel 靜態 / Netlify）——不支援長連線 WebSocket。

### 本地預覽
```bash
npm install
DEVICE_SECRET=test123 ADMIN_KEY=8888 npm start
# 打開 http://localhost:3000
```

## 二、環境變數

| 變數 | 說明 | 預設 |
|---|---|---|
| `PORT` | 服務埠（平台通常自動注入） | `3000` |
| `DEVICE_SECRET` | 設備密鑰：**必須與 ESP32 草稿碼 `SERVER_URL` 中的 secret 相同** | `change-me-device-secret` |
| `ADMIN_KEY` | 管理密碼：網站上開門/清空/對位等操作需要 | `8888` |

## 三、部署後的接線步驟（重要）

1. **記下部署網址與 DEVICE_SECRET**（Render 後台可見；render.yml 會自動產生一組 DEVICE_SECRET）。
2. 打開 ESP32 草稿碼 `esp32-firmware/umbrella_cabinet_online.ino`，修改：
   ```cpp
   const char* SERVER_URL = "wss://你的網址/ws?role=device&secret=你的DEVICE_SECRET";
   ```
3. 用 Arduino IDE 重新上傳到 ESP32（序列埠 115200 應看到「已連接中繼伺服器，在線控制就緒」）。
4. **製作櫃面 QR 碼**，內容就是網站網址（純文字或 URL 皆可），打印過膠貼上。
5. 手機（不限網絡，流動數據也行）掃碼 → 存傘／取傘。

## 四、管理與除錯

- 網站底部先在「管理員功能」輸入 `ADMIN_KEY` 解鎖；解鎖後才會顯示「管理／除錯」與「即時指令監控」。可進行開門、關門、清空全部、對位（home）、轉到指定格（go），並查看各格取傘碼；完成後可重新鎖定。
- 前端 v2 提供手機數字鍵盤、深色模式、PWA 主畫面安裝、離線殼、Toast／震動回饋與斷線自動重連；網站連線中斷時，尚未送出的指令會在恢復後補送。
- 取傘除手動輸入 4 位碼外，還支援本機保留碼一鍵填入，以及手機原生分享／不支援時自動複製取傘碼；頂部導覽可快速跳到櫃況、存傘、取傘與管理區。
- 健康檢查：`GET /healthz`（回傳設備在線狀態）、`GET /api/status`。
- 櫃子一次只處理一個動作；網站會顯示「櫃子運作中」。
- ESP32 端保留本地備援：連 `Umbrella` 熱點（密碼 12345678）開 `http://192.168.4.1`，以及序列埠指令（status / scan / led test / open / close / step / go / home / clear）。
- ESP32 機身指示燈：存傘慢閃／取傘快閃／開門常亮／完成三連閃；待機時每 3 秒輕閃（離線時快閃警示）。

## 五、注意事項

- **單設備設計**：同一把密鑰只允許一台 ESP32 在線，新裝置連入會踢掉舊連線。
- **免費 tier 休眠**：Render 免費版約 15 分鐘無流量會休眠，下次連線需 30–60 秒喚醒；ESP32 會自動重連。在意喚醒時間可用 UptimeRobot 每 5 分鐘 ping `/healthz`，或改付費方案。
- **伺服器穩定性**：網站操作會由伺服器序列化後逐一送給 ESP32，避免多個手機同時點擊造成命令交錯；每個命令有 45 秒逾時，設備斷線或逾時會清理佇列。逾時後需等 ESP32 重新連線，避免晚到回應誤套用到下一個操作。
- **Autoscale 提醒**：Manus Autoscale 閒置時可能縮放；ESP32 韌體每 5 秒自動重連。需要完全不因縮放中斷的長連線時，請改用具備 Reserved／always-on 資格的託管方案。
- **安全**：DEVICE_SECRET 與 ADMIN_KEY 請勿使用預設值；QR 一旦外流，知道 ADMIN_KEY 就能操作櫃子（與原本地版相同的安全級別）。
- 斷電重啟後取傘碼不會遺失（存於 ESP32 快閃記憶體）。

## 六、檔案結構

```
├── server.js            中繼伺服器（Express 靜態託管 + WebSocket 中繼）
├── public/
│   ├── index.html       手機網頁（存傘/取傘/管理）
│   ├── app.js           前端邏輯（WebSocket + 自動重連 + 離線緩衝）
│   ├── sw.js             PWA Service Worker 離線殼
│   ├── manifest.webmanifest  PWA 安裝資訊
│   └── icon.svg          PWA 圖標
├── esp32-firmware/
│   ├── umbrella_cabinet_online.ino   ESP32 草稿碼（v4.2 在線控制版：WPA2/WPA3 雙認證＋操作指示燈）
│   └── README.md         燒錄說明（含指示燈接線、WPA3 兼容性說明）
├── test/                本地測試工具（模擬 ESP32／模擬用戶，無需硬件即可驗證）
├── MANUS_DEPLOY.md      部署到 Manus 的專用指南（含給 Manus 的指令）
├── render.yaml          Render 一鍵部署配置
└── .env.example         環境變數範本
```
