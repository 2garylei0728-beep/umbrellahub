/* 模擬 ESP32（真實協議，v4.2 韌體實際行為）：
   下行（伺服器→設備）= 純文字命令：status / store / take 1234 / open / close / clear / home N / go N
   上行（設備→伺服器）= 不帶 id 的 JSON：{t:'status',...} / {t:'result',ok,msg,slot,code}
   韌體行為要點（與 server.js 的配對邏輯對應）：
   - store：先推 result(ok,slot,code)，接著 busy=true，約 6 秒後 busy=false（不補發 result）
   - take 成功：只推 busy 狀態週期，不推 result（由伺服器依 busy 結束推斷成功）
   - take 失敗：立即推 result(ok:false, msg:'取傘碼錯誤')
   - open/close/clear/home/go：立即推 result */
'use strict';
const WebSocket = require('ws');

const URL = process.env.WS_URL || 'ws://127.0.0.1:3000/ws';
const SECRET = process.env.DEVICE_SECRET || 'change-me-device-secret';

const SLOTS = 8;
let codes = new Array(SLOTS).fill('');
let cur = 0;
let busy = false;
let jobType = 0;   // 1=存傘 2=取傘（模擬 runJob 非阻塞狀態機）
let nextCode = 1000;

function send(obj) {
  if (ws.readyState === 1) ws.send(JSON.stringify(obj));
}
function pushStatus() {
  send({ t: 'status', free: codes.filter(c => c === '').length, busy, cur: cur + 1, codes });
}
function pushResult(ok, msg, slot, code) {
  send({ t: 'result', ok, msg: msg || '', slot: slot || 0, code: code || '' });
}

const ws = new WebSocket(URL + '?role=device&secret=' + encodeURIComponent(SECRET));

ws.on('open', () => {
  console.log('[mock-esp32] 已連接伺服器（真實協議：文字下行／無 id JSON 上行）');
  pushStatus();
  setInterval(pushStatus, 2000);   // 韌體每 2 秒推送狀態
});

/* 非阻塞任務狀態機（模擬 runJob）：忙碌 6 秒 */
function startJob(type) {
  busy = true;
  jobType = type;
  pushStatus();
  setTimeout(() => {
    busy = false;
    jobType = 0;
    pushStatus();
  }, 6000);
}

ws.on('message', (raw) => {
  const text = raw.toString();
  let m = null;
  try { m = JSON.parse(text); } catch (e) { /* 純文字命令 */ }
  if (m) return;   // 韌體忽略 JSON（如伺服器的 hello）

  const cmd = text.trim();
  console.log('[mock-esp32] 收到文字命令:', JSON.stringify(cmd));

  if (cmd === 'status') { pushStatus(); return; }

  if (cmd === 'store') {
    if (busy || jobType) { pushResult(false, '櫃子運作中，請稍後再試'); return; }
    const k = codes.findIndex(c => c === '');
    if (k < 0) { pushResult(false, '櫃子已滿'); return; }
    const code = String(nextCode++);
    codes[k] = code;
    pushResult(true, '', k + 1, code);   // 先給結果（帶取傘碼）
    pushStatus();
    startJob(1);                          // 再進入忙碌週期
    return;
  }

  if (cmd.startsWith('take ')) {
    const code = cmd.substring(5).trim();
    if (busy || jobType) { pushResult(false, '櫃子運作中，請稍後再試'); return; }
    const k = code ? codes.findIndex(c => c === code) : -1;
    if (k < 0) { pushResult(false, '取傘碼錯誤'); return; }
    codes[k] = '';                        // 取傘成功：清格（韌體在開門取走後清）
    pushStatus();
    startJob(2);                          // 只推忙碌週期，不補 result
    return;
  }

  if (cmd === 'open') {
    if (busy || jobType) { pushResult(false, '櫃子運作中'); return; }
    pushResult(true, '門已開');
    return;
  }
  if (cmd === 'close') {
    if (busy || jobType) { pushResult(false, '櫃子運作中'); return; }
    pushResult(true, '門已關');
    return;
  }
  if (cmd === 'clear') {
    codes = new Array(SLOTS).fill('');
    pushResult(true, '已清空全部格位');
    pushStatus();
    return;
  }
  if (cmd.startsWith('home ')) {
    const n = parseInt(cmd.substring(5), 10);
    if (n >= 1 && n <= SLOTS) { cur = n - 1; pushResult(true, '已對位：第 ' + n + ' 格'); pushStatus(); }
    else pushResult(false, '格號必須是 1-' + SLOTS);
    return;
  }
  if (cmd.startsWith('go ')) {
    const n = parseInt(cmd.substring(3), 10);
    if (busy || jobType) { pushResult(false, '櫃子運作中'); return; }
    if (n >= 1 && n <= SLOTS) {
      setTimeout(() => { cur = n - 1; pushResult(true, '已轉到第 ' + n + ' 格'); pushStatus(); }, 1500);
    } else pushResult(false, '格號必須是 1-' + SLOTS);
    return;
  }

  console.log('[mock-esp32] 未知命令（韌體同樣忽略）:', JSON.stringify(cmd));
});

ws.on('close', () => { console.log('[mock-esp32] 連線關閉'); process.exit(0); });
ws.on('error', (e) => console.log('[mock-esp32] 錯誤:', e.message));
