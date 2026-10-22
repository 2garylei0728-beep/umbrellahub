/* 寬限隊列專項測試：設備離線時下指令 → 應進入隊列而非即時報錯；
   設備在寬限期內重連 → 指令補送並完成。用法：
   WS_URL=ws://127.0.0.1:3100/ws node test_grace_queue.js   （需先手動殺掉 mock_esp32）*/
'use strict';
const WebSocket = require('ws');

const URL = process.env.WS_URL || 'ws://127.0.0.1:3100/ws';
const ws = new WebSocket(URL + '?role=user');

let id = 0;
const pending = new Map();

ws.on('open', () => {
  console.log('[grace-test] 已連線（此時設備應為離線狀態）');
  const mid = ++id;
  const timer = setTimeout(() => { console.log('❌ 超時：指令未被補送'); process.exit(1); }, 20000);
  pending.set(mid, { resolve: (m) => { clearTimeout(timer); console.log('✅ 指令結果:', JSON.stringify(m)); process.exit(0); } });
  console.log('[grace-test] 送出「存傘」指令（設備離線中）…');
  ws.send(JSON.stringify({ t: 'store', id: mid }));
  console.log('[grace-test] 已送出——請在 5 秒內重新啟動 mock_esp32');
});

ws.on('message', (raw) => {
  let m;
  try { m = JSON.parse(raw.toString()); } catch (e) { return; }
  if (m.t === 'status') console.log('[grace-test] 狀態: 在線=', m.online);
  if (m.t === 'presence') console.log('[grace-test] presence: 設備', m.online ? '上線' : '離線');
  if (m.t === 'result') {
    const p = pending.get(m.id);
    if (p) { pending.delete(m.id); p.resolve(m); }
  }
});

ws.on('close', () => console.log('[grace-test] 連線關閉'));
ws.on('error', (e) => console.log('[grace-test] 錯誤:', e.message));
