# 智能雨傘寄存櫃在線控制：部署計畫

## 實作方式

沿用上傳專案的 Node.js Express + `ws` 中繼架構。Express 提供 `public/` 下的手機控制頁面、`/healthz` 與 `/api/status`；WebSocket 伺服器在 `/ws` 同時接收瀏覽器與 ESP32 連線。ESP32 使用受保護的 `DEVICE_SECRET` 驗證，瀏覽器的管理操作使用 `ADMIN_KEY` 驗證。

由於原有 ESP32 韌體的下行協定是純文字命令（`store`、`take 1234`、`open`、`close`、`clear`、`home N`、`go N`），伺服器會把瀏覽器的 JSON 操作轉成該格式；ESP32 回傳的狀態與結果仍依照既有 JSON 格式廣播到瀏覽器。單設備一次一個動作，因此伺服器會暫存目前瀏覽器請求的 id，將韌體沒有攜帶 id 的結果正確配回原請求。

採用 Docker 容器發布，執行 `npm start` 並監聽 Manus 注入的 `PORT`。託管模式使用 Reserved／`always_on`，避免 ESP32 的 WSS 長連線因閒置縮放而中斷。

穩定性優化加入單一飛行命令與最多 16 個待處理命令的佇列、45 秒命令逾時、設備斷線／替換時的完整清理、使用者斷線時取消尚未送出的操作，以及逾時後等待 ESP32 重連來隔離晚到回應。WebSocket 限制最大訊息大小、關閉不必要的壓縮並啟用 TCP no-delay；HTTP 伺服器也設定合理的 keep-alive 與 headers timeout。

## 專案結構

- `server.js`：Express、健康端點、瀏覽器 WebSocket 與 ESP32 WebSocket 中繼。
- `public/index.html`、`public/app.js`：手機／電腦控制介面。
- `esp32-firmware/`：部署後填入 WSS 網址與設備密鑰的 Arduino 韌體及燒錄說明。
- `Dockerfile`、`package.json`、`package-lock.json`：生產容器與依賴。
- `README.md`、`MANUS_DEPLOY.md`：使用、燒錄與部署說明。

## 設計方向

介面維持原專案的「清楚的現場控制台」：白色卡片、低干擾灰底、藍色主要操作、綠／紅在線狀態。設計重點是大按鈕、手機單手操作、狀態先於裝飾，適合雨傘櫃現場快速存取。
