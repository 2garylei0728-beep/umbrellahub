/* 智能雨傘寄存櫃 — 網站前端邏輯
   與中繼伺服器走 WebSocket（/ws?role=user），斷線時退化为 /api/status 輪詢（唯讀） */
'use strict';

const $ = (id) => document.getElementById(id);

/* ---------- 狀態 ---------- */
let ws = null;
let wsOpen = false;
let reconnectDelay = 1000;
let msgId = 0;
let lastStatus = { online: false, free: 8, total: 8, busy: false, cur: 1, codes: [] };
const pending = new Map();   // id → { resolve, timer }
let idleResolvers = [];      // 等待 busy 結束的 promise

/* ---------- 連線 ---------- */
function setConn(cls, text) {
  const el = $('conn');
  el.className = 'pill ' + cls;
  el.textContent = text;
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws?role=user');

  ws.onopen = () => {
    wsOpen = true;
    reconnectDelay = 1000;
    stopStatusPoll();
    setConn(lastStatus.online ? 'on' : 'off', lastStatus.online ? '櫃子在線' : '櫃子離線');
  };

  ws.onclose = () => {
    wsOpen = false;
    failAllPending('與伺服器斷開，請稍後再試');
    setConn('', '重新連接中…');
    startStatusPoll();
    setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 5000);
  };

  ws.onerror = () => { try { ws.close(); } catch (e) { /* 忽略 */ } };

  ws.onmessage = (e) => {
    let m;
    try { m = JSON.parse(e.data); } catch (err) { return; }
    handle(m);
  };
}

/* ---------- 訊息處理 ---------- */
function handle(m) {
  if (m.t === 'status') {
    lastStatus = { online: m.online !== false, free: m.free, total: m.total || 8, busy: !!m.busy, cur: m.cur || 1, codes: m.codes || [] };
    renderStatus();
    if (!lastStatus.busy) flushIdle();
  } else if (m.t === 'presence') {
    lastStatus.online = m.online;
    renderStatus();
    setConn(m.online ? 'on' : 'off', m.online ? '櫃子在線' : '櫃子離線');
  } else if (m.t === 'result') {
    const p = pending.get(m.id);
    if (p) {
      clearTimeout(p.timer);
      pending.delete(m.id);
      p.resolve(m);
    }
  }
}

function renderStatus() {
  const busyTxt = lastStatus.busy ? '（櫃子運作中…）' : '';
  $('st').textContent = '空格：' + lastStatus.free + ' / ' + lastStatus.total + busyTxt +
    (lastStatus.online ? '' : '　⚠️ 櫃子離線');
  $('bs').disabled = !lastStatus.online || lastStatus.busy;
  $('bt').disabled = !lastStatus.online || lastStatus.busy;
  renderSlotTable();
}

function renderSlotTable() {
  const t = $('slotTable');
  let html = '<tr><th>格位</th><th>取傘碼</th></tr>';
  const codes = lastStatus.codes || [];
  for (let i = 0; i < (lastStatus.total || 8); i++) {
    const c = codes[i];
    html += '<tr><td>第 ' + (i + 1) + ' 格' + (i === (lastStatus.cur - 1) ? '（對正）' : '') + '</td><td>' +
      (c && c !== '' ? c : '空') + '</td></tr>';
  }
  t.innerHTML = html;
}

/* ---------- 動作 ---------- */
function send(obj) {
  return new Promise((resolve, reject) => {
    if (!wsOpen) { reject(new Error('offline')); return; }
    const id = ++msgId;
    obj.id = id;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error('timeout')); }, 25000);
    pending.set(id, { resolve, timer });
    ws.send(JSON.stringify(obj));
  });
}

function failAllPending(msg) {
  for (const [, p] of pending) {
    clearTimeout(p.timer);
    try { p.reject(new Error(msg || '離線')); } catch (e) { /* 忽略 */ }
  }
  pending.clear();
}

function setMsg(t) { $('m').textContent = t; }
function setMsgHTML(h) { $('m').innerHTML = h; }

function waitIdle(maxMs) {
  return new Promise((resolve) => {
    if (!lastStatus.busy) { resolve(); return; }
    const timer = setTimeout(() => { idleResolvers = idleResolvers.filter(r => r !== done); resolve(); }, maxMs || 60000);
    const done = () => { clearTimeout(timer); resolve(); };
    idleResolvers.push(done);
  });
}
function flushIdle() {
  const rs = idleResolvers;
  idleResolvers = [];
  rs.forEach(r => r());
}

async function store() {
  setMsg('請稍候…');
  try {
    const r = await send({ t: 'store' });
    if (!r.ok) { setMsg(r.msg || '存傘失敗'); return; }
    try { localStorage.setItem('umbrellaCode', r.code); } catch (e) { /* 忽略 */ }
    $('cd').value = r.code;
    setMsgHTML('第 ' + r.slot + ' 格開門中，請放入雨傘<br>你的取傘碼<div class="big">' + r.code + '</div>請截圖或抄下');
    await waitIdle();
    setMsgHTML($('m').innerHTML + '<br>✅ 完成，門已關閉');
  } catch (e) {
    setMsg(e.message === 'timeout' ? '櫃子沒有回應，請重新整理再試' : '連線中斷，請稍後再試');
  }
}

async function take() {
  const code = $('cd').value.trim();
  if (!/^\d{4}$/.test(code)) { setMsg('請輸入 4 位取傘碼'); return; }
  setMsg('請稍候…');
  try {
    const r = await send({ t: 'take', code });
    if (!r.ok) { setMsg(r.msg || '取傘失敗'); return; }
    try { localStorage.removeItem('umbrellaCode'); } catch (e) { /* 忽略 */ }
    setMsg('第 ' + r.slot + ' 格開門中，請取走雨傘');
    await waitIdle();
    setMsg('✅ 完成，歡迎再用');
    $('cd').value = '';
  } catch (e) {
    setMsg(e.message === 'timeout' ? '櫃子沒有回應，請重新整理再試' : '連線中斷，請稍後再試');
  }
}

/* ---------- 管理 ---------- */
function adminKey() { return $('ak').value.trim() || localStorage.getItem('umbrellaAdminKey') || ''; }

async function admin(act, n) {
  try {
    const r = await send({ t: 'admin', key: adminKey(), act, n: n || 0 });
    if (r.ok) {
      try { localStorage.setItem('umbrellaAdminKey', adminKey()); } catch (e) { /* 忽略 */ }
    }
    return r;
  } catch (e) {
    return { ok: false, msg: '連線中斷' };
  }
}

/* ---------- 斷線時的唯讀輪詢 ---------- */
let pollTimer = null;
function startStatusPoll() {
  if (pollTimer) return;
  const poll = async () => {
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      const s = await r.json();
      lastStatus = { online: !!s.online, free: s.free, total: s.total || 8, busy: !!s.busy, cur: s.cur || 1, codes: s.codes || [] };
      renderStatus();
      setConn(s.online ? 'on' : 'off', s.online ? '櫃子在線（只讀）' : '櫃子離線');
    } catch (e) { setConn('', '連接中…'); }
  };
  poll();
  pollTimer = setInterval(poll, 5000);
}
function stopStatusPoll() { if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } }

/* ---------- 初始化 ---------- */
window.addEventListener('DOMContentLoaded', () => {
  let saved = '';
  try { saved = localStorage.getItem('umbrellaCode') || ''; } catch (e) { /* 忽略 */ }
  $('cd').value = saved;
  try { $('ak').value = localStorage.getItem('umbrellaAdminKey') || ''; } catch (e) { /* 忽略 */ }

  $('bs').addEventListener('click', store);
  $('bt').addEventListener('click', take);
  $('aopen').addEventListener('click', async () => { const r = await admin('open'); setMsg(r.ok ? '門已開' : (r.msg || '失敗')); });
  $('aclose').addEventListener('click', async () => { const r = await admin('close'); setMsg(r.ok ? '門已關' : (r.msg || '失敗')); });
  $('aclear').addEventListener('click', async () => { const r = await admin('clear'); setMsg(r.ok ? '已清空全部格位' : (r.msg || '失敗')); });

  for (let i = 1; i <= 8; i++) {
    const b1 = document.createElement('button');
    b1.className = 'mini';
    b1.textContent = i;
    b1.addEventListener('click', async () => { const r = await admin('home', i); setMsg(r.ok ? ('已對位：第 ' + i + ' 格') : (r.msg || '失敗')); });
    $('homeGrid').appendChild(b1);

    const b2 = document.createElement('button');
    b2.className = 'mini';
    b2.textContent = i;
    b2.addEventListener('click', async () => { const r = await admin('go', i); setMsg(r.ok ? ('已轉到第 ' + i + ' 格') : (r.msg || '失敗')); });
    $('goGrid').appendChild(b2);
  }

  renderStatus();
  connect();

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !wsOpen) {
      clearTimeout(connect._t);
      connect._t = setTimeout(connect, 100);
    }
  });
});
