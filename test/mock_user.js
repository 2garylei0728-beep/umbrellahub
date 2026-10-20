/* 模擬手機用戶：連接中繼伺服器，依序測試 store / take / 錯誤碼 / 管理操作（本地測試用） */
'use strict';
const WebSocket = require('ws');

const URL = process.env.WS_URL || 'ws://127.0.0.1:3000/ws';
const ADMIN_KEY = process.env.ADMIN_KEY || '8888';

let id = 0;
const pending = new Map();
let savedCode = null;

function send(obj) {
  return new Promise((resolve, reject) => {
    const mid = ++id;
    obj.id = mid;
    const timer = setTimeout(() => { pending.delete(mid); reject(new Error('timeout')); }, 30000);
    pending.set(mid, { resolve, timer });
    ws.send(JSON.stringify(obj));
  });
}

const ws = new WebSocket(URL + '?role=user');

ws.on('open', async () => {
  console.log('[mock-user] 已連接，開始測試流程');
  try {
    // 1. 等初始 status
    await new Promise(r => setTimeout(r, 500));
    console.log('1️⃣  初始狀態: 空格', lastStatus.free, '/', lastStatus.total);

    // 2. 存傘
    console.log('2️⃣  送出「存傘」指令…');
    const r1 = await send({ t: 'store' });
    console.log('   結果:', JSON.stringify(r1));
    savedCode = r1.code;

    // 3. 錯誤取傘碼
    console.log('3️⃣  用錯誤取傘碼取傘（預期失敗）…');
    const r2 = await send({ t: 'take', code: '9999' });
    console.log('   結果:', JSON.stringify(r2));

    // 4. 管理：錯誤密碼（預期失敗）
    console.log('4️⃣  管理操作 + 錯誤密碼（預期失敗）…');
    const r3 = await send({ t: 'admin', key: 'wrong', act: 'open' });
    console.log('   結果:', JSON.stringify(r3));

    // 5. 管理：正確密碼開門
    console.log('5️⃣  管理操作 + 正確密碼「開門」…');
    const r4 = await send({ t: 'admin', key: ADMIN_KEY, act: 'open' });
    console.log('   結果:', JSON.stringify(r4));

    // 6. 管理：對位 home 3
    console.log('6️⃣  管理「對位第 3 格」…');
    const r5 = await send({ t: 'admin', key: ADMIN_KEY, act: 'home', n: 3 });
    console.log('   結果:', JSON.stringify(r5));

    // 7. 管理：轉到 go 5
    console.log('7️⃣  管理「轉到第 5 格」…');
    const r6 = await send({ t: 'admin', key: ADMIN_KEY, act: 'go', n: 5 });
    console.log('   結果:', JSON.stringify(r6));

    // 8. 用正確取傘碼取傘
    console.log('8️⃣  用正確取傘碼 ' + savedCode + ' 取傘…');
    const r7 = await send({ t: 'take', code: String(savedCode) });
    console.log('   結果:', JSON.stringify(r7));

    // 9. 清空
    console.log('9️⃣  管理「清空全部」…');
    const r8 = await send({ t: 'admin', key: ADMIN_KEY, act: 'clear' });
    console.log('   結果:', JSON.stringify(r8));

    console.log('✅ 全部測試流程完成');
    ws.close();
    process.exit(0);
  } catch (e) {
    console.log('❌ 測試失敗:', e.message);
    process.exit(1);
  }
});

let lastStatus = { free: 0, total: 8 };

ws.on('message', (raw) => {
  let m;
  try { m = JSON.parse(raw.toString()); } catch (e) { return; }
  if (m.t === 'status') {
    lastStatus = m;
    console.log('[mock-user] 狀態更新: 空格', m.free, '忙碌:', m.busy, '對正:', m.cur, '碼:', JSON.stringify(m.codes));
  } else if (m.t === 'result') {
    const p = pending.get(m.id);
    if (p) { clearTimeout(p.timer); pending.delete(m.id); p.resolve(m); }
  } else if (m.t === 'presence') {
    console.log('[mock-user] 設備在線狀態:', m.online);
  }
});

ws.on('close', () => console.log('[mock-user] 連線關閉'));
ws.on('error', (e) => console.log('[mock-user] 錯誤:', e.message));
