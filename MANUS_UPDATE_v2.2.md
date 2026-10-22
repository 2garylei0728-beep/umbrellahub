# Manus 更新包 v2.2——連線穩定性＋速度優化（2026-10-22）

## 這次修什麼（用戶實測回饋：時連時斷、延遲高、指令慢）

### 1. 根治「時連時斷」（最重要）
**實測根因**：伺服器 heartbeat 每 30 秒 ping 所有連線，若客戶端 30 秒內沒回 pong，
下一次檢查（第 60 秒）就 `terminate()`。ESP32 的 arduinoWebSockets 客戶端在這個
場景下沒有回 pong，導致**每約 1-3 分鐘被伺服器主動掐斷一次**——這就是網站上
「櫃子狀態時連時斷」的來源（串口實證：WS 單方面斷開、WiFi 毫無事件）。

**修法**：heartbeat 改以「最後訊息時間」判定（與 watchdog 同源）——
ESP32 每 1.5 秒推 status 就是保活，不再因 ping/pong 兼容問題誤殺；
watchdog（90 秒無訊息終止）作為真正的死連線兜底。

### 2. 指令提速（固件，已另行燒錄，隨包同步）
- 步進脈衝 1000us → 800us（轉盤快 20%，TMC2209 負載實測不失步）
- 開門等待 5000ms → 3500ms（放傂 3.5 秒足夠）
- WS 斷線重連 5s → 1.5s
- status 推送 2s → 1.5s
→ 單次存傂完整周期從約 10 秒降到約 7 秒

### 3. 網頁體驗（隨包上線）
- 離線寬限：online=false 持續 3 秒才判定離線（瞬時抖動不再嚇用戶、不誤禁用按鈕）
- 恢復在線立即生效
- 按鈕即時反饋文案：「指令已發送，櫃子運作中…」

## 請替換的檔案（本包已全部更新，直接整包上傳即可）

以「umbrellahub (1).zip」（v2.0 基線）為底合併，主要變更：

- server.js          heartbeat 修復＋watchdog 90s＋指令監控廣播（cmdlog）
- public/app.js      離線寬限＋即時反饋＋指令監控渲染
- public/index.html  指令監控面板＋傂字 HTML 實體化
- public/sw.js       app.js 改網絡優先（消滅緩存舊碼）
- public/manifest.webmanifest、public/icon.svg  字元硬化
- esp32-firmware/umbrella_cabinet_online.ino  速度優化版固件（已燒錄，僅存檔同步）
- test/              真實協議 mock＋瀏覽器模擬＋監控腳本
- 新增文檔          PROTOCOL.md、OPERATION_GUIDE.md/html

## 韌體已由我們本地燒錄（無需 Manus 動作）
速度優化版固件（v4.2-online, STEP_DELAY 800/DOOR_HOLD 3500/WS 重連 1.5s）已燒錄實測。
.ino 僅作存檔同步，不需要 Manus 端編譯。

## 預期效果
- 櫃子連線持續穩定，不再定期掉線
- 網站離線提示不再亂閃
- 指令周期縮短約 30%
