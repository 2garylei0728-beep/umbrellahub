/* 模擬 ESP32：連接中繼伺服器，回應 store/take/admin 指令（本地測試用） */
'use strict';
const WebSocket = require('ws');

const URL = process.env.WS_URL || 'ws://127.0.0.1:3000/ws';
const SECRET = process.env.DEVICE_SECRET || 'change-me-device-secret';

const SLOTS = 8;
let codes = new Array(SLOTS).fill('');
let cur = 1;
let busy = false;
let nextCode = 1000;

function pushStatus() {
  send({ t: 'status', free: codes.filter(c => c === '').length, busy, cur, codes });
}
function send(obj) {
  if (ws.readyState === 1) ws.send(JSON.stringify(obj));
}

const ws = new WebSocket(URL + '?role=device&secret=' + encodeURIComponent(SECRET));

ws.on('open', () => {
  console.log('[mock-esp32] 已連接伺服器');
  pushStatus();
  setInterval(pushStatus, 2000);
});

ws.on('message', (raw) => {
  const command = raw.toString().trim();
  if (command.startsWith('{')) return; // 忽略伺服器 hello
  console.log('[mock-esp32] 收到指令:', command);

  if (command === 'store') {
    if (busy) { send({ t: 'result', ok: false, msg: '櫃子運作中，請稍後再試' }); return; }
    const k = codes.findIndex(c => c === '');
    if (k < 0) { send({ t: 'result', ok: false, msg: '櫃子已滿' }); return; }
    const code = String(nextCode++);
    codes[k] = code;
    busy = true;
    pushStatus();
    setTimeout(() => { // 模擬轉盤 + 開門 5 秒
      busy = false;
      pushStatus();
      send({ t: 'result', ok: true, slot: k + 1, code });
    }, 6000);
  } else if (command.startsWith('take ')) {
    const code = command.slice(5).trim();
    if (busy) { send({ t: 'result', ok: false, msg: '櫃子運作中，請稍後再試' }); return; }
    const k = codes.findIndex(c => c === code);
    if (k < 0) { send({ t: 'result', ok: false, msg: '取傘碼錯誤' }); return; }
    busy = true;
    pushStatus();
    setTimeout(() => {
      codes[k] = '';
      busy = false;
      pushStatus();
      send({ t: 'result', ok: true, slot: k + 1 });
    }, 6000);
  } else if (command === 'open' || command === 'close') {
    if (busy) { send({ t: 'result', ok: false, msg: '櫃子運作中' }); return; }
    send({ t: 'result', ok: true, msg: command === 'open' ? '門已開' : '門已關' });
  } else if (command === 'clear') {
    if (busy) { send({ t: 'result', ok: false, msg: '櫃子運作中' }); return; }
      codes = new Array(SLOTS).fill('');
      pushStatus();
      send({ t: 'result', ok: true, msg: '已清空全部格位' });
  } else if (command.startsWith('home ')) {
      const n = Number(command.slice(5));
      if (!Number.isInteger(n) || n < 1 || n > SLOTS) { send({ t: 'result', ok: false, msg: '格號必須是 1-' + SLOTS }); return; }
      cur = n;
      pushStatus();
      send({ t: 'result', ok: true, msg: '已對位：第 ' + n + ' 格' });
  } else if (command.startsWith('go ')) {
      const n = Number(command.slice(3));
      if (!Number.isInteger(n) || n < 1 || n > SLOTS) { send({ t: 'result', ok: false, msg: '格號必須是 1-' + SLOTS }); return; }
      if (busy) { send({ t: 'result', ok: false, msg: '櫃子運作中' }); return; }
      busy = true;
      pushStatus();
      setTimeout(() => { cur = n; busy = false; pushStatus(); send({ t: 'result', ok: true, msg: '已轉到第 ' + n + ' 格' }); }, 2000);
  } else {
    send({ t: 'result', ok: false, msg: '不支援的操作' });
  }
});

ws.on('close', () => { console.log('[mock-esp32] 連線關閉'); process.exit(0); });
ws.on('error', (e) => { console.log('[mock-esp32] 錯誤:', e.message); });
