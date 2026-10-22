/* 真櫃指令速度實測：連真中繼，計時 store→拿碼→take→完整周期 */
'use strict';
const WebSocket = require('ws');
const URL = 'wss://umbrellahub-caozd3vf.manus.space/ws?role=user';
const ws = new WebSocket(URL, { origin: 'https://umbrellahub-caozd3vf.manus.space' });
let t0 = 0, tCode = 0, tDone = 0, code = '', phase = 'idle', seq = 0;

function send(o) { ws.send(JSON.stringify(o)); }
function ms(a, b) { return ((b - a) / 1000).toFixed(2) + 's'; }

ws.on('open', () => {
  console.log('已連上真中繼');
  seq = 1; t0 = Date.now();
  console.log('[' + new Date().toTimeString().slice(0, 8) + '] 發送 store（計時開始）');
  send({ t: 'store' });
});
ws.on('message', (raw) => {
  const m = JSON.parse(raw.toString());
  if (m.t === 'status') {
    if (seq === 1 && m.busy && !tCode) { /* 轉動中 */ }
    if (seq === 1 && !m.busy && tCode && !tDone) {
      tDone = Date.now();
      console.log('  完整周期（轉盤+開門+關門）: ' + ms(t0, tDone));
      // 接著測 take
      seq = 2; t0 = Date.now();
      console.log('[' + new Date().toTimeString().slice(0, 8) + '] 發送 take ' + code + '（計時開始）');
      send({ t: 'take', code: code });
    }
    if (seq === 2 && !m.busy && tDone) {
      console.log('  take 完整周期: ' + ms(t0, Date.now()));
      console.log('=== 實測完畢 ===');
      process.exit(0);
    }
  } else if (m.t === 'result') {
    if (m.ok && m.code) {
      tCode = Date.now();
      code = m.code;
      console.log('  ✅ 拿到取傂碼 ' + code + '（第' + m.slot + '格）→ 用戶拿到碼耗時: ' + ms(t0, tCode));
    } else if (seq === 2) {
      console.log('  take 回執: ' + m.msg + '（耗時 ' + ms(t0, Date.now()) + '）');
      setTimeout(() => process.exit(0), 1500);
    } else if (!m.ok) {
      console.log('  ❌ ' + m.msg);
      process.exit(1);
    }
  }
});
setTimeout(() => { console.log('超時退出'); process.exit(2); }, 60000);
