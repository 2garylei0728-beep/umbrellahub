# 部署到 Manus（專用指南）

本專案（Node.js 中繼伺服器 + 手機網站）**可以直接託管在 Manus 上**。最佳設定是 Reserved（預留實例）；若帳戶只能使用 Autoscale，系統仍可運作，ESP32 會自動重連，但閒置後第一次操作可能遇到冷啟動。

## 為什麼必須選 Reserved 模式

| 託管模式 | 運行方式 | WebSocket 長連線 |
|---|---|---|
| Autoscale（預設） | Google Cloud Run，無流量時縮放到零，有冷啟動 | ⚠️ 會中斷 ESP32 長連線；韌體每 5 秒自動重連 |
| **Reserved（本專案要用）** | 常駐進程，不縮放、無冷啟動、無請求超時 | ✅ 官方明確支援：WebSocket 連線、背景進程、實時資料流 |

Reserved 是否可用取決於 Manus 帳戶資格；Autoscale 則不需要 Reserved 資格。

## 一、給 Manus 的指令（直接複製貼上）

> 我上傳了一個 Node.js 專案（雨傘櫃在線控制系統），結構是：
> - `server.js`：Express + WebSocket 中繼伺服器，同時託管靜態網站
> - `public/`：手機控制網頁（index.html + app.js）
> - `package.json`：依賴 express 與 ws
>
> 請用這個專案建立網站並發布。要求：
> 1. **若帳戶具備資格，託管模式選擇 Reserved（預留實例）**；若只能使用 Autoscale，ESP32 仍會透過每 5 秒自動重連恢復連線。
> 2. 建置指令 `npm install`，啟動指令 `npm start`，服務埠讀取環境變數 `PORT`。
> 3. 設定環境變數：`DEVICE_SECRET=請幫我產生一組隨機密鑰`、`ADMIN_KEY=8888`。
> 4. 保留 `/healthz` 健康檢查端點。
> 5. 發布後把**網站網址**和**DEVICE_SECRET 的值**告訴我。

如果 Manus 詢問用途，補充說明：這是一個 IoT 中繼伺服器，一台 ESP32（雨傘寄存櫃）通過 WSS 長連線連入，手機用戶通過網站下達存傘/取傘指令，伺服器負責轉發與狀態廣播。

## 二、Manus 部署後的接線步驟

1. **拿到網址與 DEVICE_SECRET**（例如 `https://umbrella-xxxx.manus.space`）。
2. 修改 ESP32 草稿码 `esp32-firmware/umbrella_cabinet_online.ino` 第 22 行：
   ```cpp
   const char* SERVER_URL = "wss://你的manus網址/ws?role=device&secret=你的DEVICE_SECRET";
   ```
   注意：Manus 自動配 SSL，網址是 `https://` 開頭，這裡就用 `wss://`（只把 https 換成 wss，其餘照抄）。
3. 用 Arduino IDE 重新上傳（Board: ESP32 Dev Module、埠: COM11、115200）。
4. 序列埠監控器應出現：
   ```
   [WS] 連接伺服器 你的主機:443/ws?role=device&secret=... ...
   [WS] 已連接中繼伺服器，在線控制就緒
   ```
5. 櫃面 QR 码內容 = Manus 網站網址，打印過膠貼上。手機掃碼（任何網絡）即可控制。

## 三、注意事項

- **地區**：Manus 託管在美國，ESP32 在澳門連過去延遲約 200ms，對存/取傘操作完全沒影響。
- **斷線自癒**：ESP32 每 5 秒自動重連；伺服器每 30 秒發送心跳保持連線活跃。
- **安全**：DEVICE_SECRET 與 ADMIN_KEY 不要用預設值；QR 外流意味著任何人可操作櫃子（與原本地版相同的安全級別，管理操作另有 ADMIN_KEY 把關）。
- **備援**：ESP32 保留本地熱點 `Umbrella`（密碼 12345678，`http://192.168.4.1`）與序列埠指令，Manus 伺服器故障時仍可本地操作。
- **修改網站**：日後要改網頁外觀或功能，把新版本專案再給 Manus 重新發布即可；ESP32 固件不用動（除非換網址或密鑰）。
