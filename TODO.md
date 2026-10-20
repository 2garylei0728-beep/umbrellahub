# 智能雨傘寄存櫃在線控制 — 交付待辦

- [x] **部署 Node.js WebSocket 中繼服務**
  - 服務以 Node.js Express 常駐進程啟動，監聽 Manus 提供的 `PORT`，並支援 `/ws` WebSocket 長連線。
  - 保留 `/healthz` 與 `/api/status` 端點。
- [x] **提供手機／電腦控制網站**
  - `public/index.html` 與 `public/app.js` 透過 HTTPS/WSS 提供存傘、取傘及管理／除錯操作。
  - 控制端顯示 ESP32 在線狀態、格位狀態與操作結果。
- [x] **串接 ESP32 韌體協定**
  - ESP32 以 `wss://.../ws?role=device&secret=...` 連入。
  - 伺服器維持既有訊息格式，將存傘、取傘與管理指令轉成韌體使用的文字命令，並廣播狀態與結果。
- [x] **保護部署設定**
  - `DEVICE_SECRET` 與 `ADMIN_KEY` 僅由 Manus 受保護環境變數提供，不寫入前端或版本庫。
- [x] **完成 Manus 發布與接線指引**
  - 發布成功後提供網站 HTTPS 網址、對應 WSS 網址，以及把設備密鑰寫回 ESP32 `SERVER_URL` 的指引。
