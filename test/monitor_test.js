/* 監控端測試：以普通用戶身份連線，只打印 cmdlog 廣播（驗證指令監控數據流） */
'use strict';
const WebSocket = require('ws');
const ws = new WebSocket((process.env.WS_URL || 'ws://127.0.0.1:3108/ws') + '?role=user');
ws.on('open', () => console.log('[monitor] 已連線，等待指令流…'));
ws.on('message', (raw) => {
  let m;
  try { m = JSON.parse(raw.toString()); } catch (e) { return; }
  if (m.t !== 'cmdlog') return;
  const t = new Date().toTimeString().slice(0, 8);
  if (m.dir === 'tx') {
    console.log('[' + t + '] 📡 ESP32 收到：' + m.cmd);
  } else {
    const extra = (m.slot ? '（第' + m.slot + '格）' : '') + (m.code ? ' 碼:' + m.code : '');
    console.log('[' + t + '] ' + (m.ok ? '✅' : '❌') + ' ESP32 回執：' + (m.msg || '') + extra);
  }
});
ws.on('error', (e) => console.log('[monitor] 錯誤:', e.message));
