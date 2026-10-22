/* 綜合測試：① 寬限期到期報錯（設備始終不重連）② 殭屍裝備判定（連上後沉默）*/
'use strict';
const WebSocket = require('ws');
const URL = process.env.WS_URL || 'ws://127.0.0.1:3100/ws';
const SECRET = process.env.DEVICE_SECRET || 'test-secret';

let pass = 0, fail = 0;
function ok(cond, name) {
  console.log((cond ? '✅' : '❌'), name);
  cond ? pass++ : fail++;
}

/* Part A：設備離線，指令等過寬限期（6s）應收到「暫時離線」報錯 */
const user = new WebSocket(URL + '?role=user');
let gotExpire = false;

user.on('open', () => {
  console.log('[A] 用戶已連線（設備離線中），送出存傘指令…');
  user.send(JSON.stringify({ t: 'store', id: 1 }));
  setTimeout(() => {
    ok(gotExpire, '寬限期到期後收到「暫時離線」報錯');
    startPartB();
  }, 9000);
});

user.on('message', (raw) => {
  let m;
  try { m = JSON.parse(raw.toString()); } catch (e) { return; }
  if (m.t === 'result' && m.id === 1) {
    gotExpire = true;
    console.log('[A] 收到結果:', JSON.stringify(m));
    ok(m.ok === false && /離線/.test(m.msg), '結果為失敗＋提示離線');
  }
});

/* Part B：沉默裝備（連上後一個字都不發）→ 8 秒後應被殭屍看守終止 */
function startPartB() {
  console.log('[B] 沉默裝備連線中（不推送任何訊息）…');
  const zombie = new WebSocket(URL + '?role=device&secret=' + encodeURIComponent(SECRET));
  let closed = false;
  zombie.on('open', () => console.log('[B] 沉默裝備已連線'));
  zombie.on('close', (code) => {
    closed = true;
    console.log('[B] 沉默裝備被終止，close code =', code);
    ok(true, '殭屍裝備被伺服器主動終止');
    finish();
  });
  setTimeout(() => {
    if (!closed) { ok(false, '殭屍裝備被伺服器主動終止（超時未終止）'); finish(); }
  }, 14000);
}

function finish() {
  console.log(`\n===== 結果：${pass} 通過 / ${fail} 失敗 =====`);
  process.exit(fail ? 1 : 0);
}

user.on('error', (e) => console.log('[A] 錯誤:', e.message));
