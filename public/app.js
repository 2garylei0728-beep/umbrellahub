/* 智能旋轉式雨僘寄存櫃 — 網站前端邏輯 v2.0（行業交付標準）
   與中繼伺服器走 WebSocket（/ws?role=user），斷線時：
   ① 指數退避自動重連；② 離線期間的指令進入緩衝，恢復後自動補發；
   ③ 退化為 /api/status 輪詢（唯讀）顯示狀態。
   協議與伺服器／ESP32 固件完全兼容，無需設備端改動。 */
'use strict';

const $ = (id) => document.getElementById(id);

/* ---------- 狀態 ---------- */
let ws = null;
let wsOpen = false;
let reconnectDelay = 1000;
let reconnectTimer = null;
let msgId = 0;
let lastStatus = { online: false, free: 8, total: 8, busy: false, cur: 1, codes: [] };
let lastStatusJson = '';
/* 離線寬限：online=false 持續 3 秒才判定離線（瞬時抖動不嚇用戶、不誤禁用按鈕）；
   恢復在線立即生效 */
let effectiveOnline = false;
let onlineGraceTimer = null;
function applyOnline(on) {
  if (on) {
    if (onlineGraceTimer) { clearTimeout(onlineGraceTimer); onlineGraceTimer = null; }
    if (!effectiveOnline) { effectiveOnline = true; refreshConn(); }
  } else if (effectiveOnline && !onlineGraceTimer) {
    onlineGraceTimer = setTimeout(() => {
      onlineGraceTimer = null;
      effectiveOnline = false;
      refreshConn();
    }, 3000);
  }
}
const pending = new Map();   // id → { resolve, reject, timer }
const buffer = [];           // 離線緩衝的指令（恢復後補發）
let idleResolvers = [];
let takeCode = '';           // 取僘碼輸入

/* ---------- Toast ---------- */
function toast(msg, type, ms) {
  const wrap = $('toastWrap');
  while (wrap.children.length >= 3) wrap.removeChild(wrap.firstChild);
  const el = document.createElement('div');
  el.className = 'toast' + (type ? ' ' + type : '');
  el.textContent = msg;
  wrap.appendChild(el);
  requestAnimationFrame(() => el.classList.add('show'));
  setTimeout(() => {
    el.classList.remove('show');
    setTimeout(() => el.remove(), 300);
  }, ms || 2600);
}

/* ---------- 震動反饋（支持的設備） ---------- */
function buzz(pattern) {
  try { if (navigator.vibrate) navigator.vibrate(pattern); } catch (e) { /* 忽略 */ }
}
const BUZZ = { tap: [8], ok: [25, 40, 25], err: [130, 60, 130] };

/* ---------- 連線狀態 ---------- */
function setPill(cls, text) {
  const el = $('conn');
  el.className = 'pill ' + cls;
  el.textContent = text;
}
function refreshConn() {
  if (!wsOpen) { setPill('wait', reconnectTimer ? '重新連接中…' : '連接中…'); return; }
  if (lastStatus.online) setPill('on', '櫃子在線');
  else setPill('off', '櫃子離線');
}

function connect() {
  clearTimeout(reconnectTimer);
  reconnectTimer = null;
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws?role=user');

  ws.onopen = () => {
    wsOpen = true;
    reconnectDelay = 1000;
    stopStatusPoll();
    refreshConn();
    flushBuffer();
  };

  ws.onclose = () => {
    wsOpen = false;
    failAllPending('與伺服器斷開，請稍後再試');
    refreshConn();
    startStatusPoll();
    scheduleReconnect();
  };

  ws.onerror = () => { try { ws.close(); } catch (e) { /* 忽略 */ } };

  ws.onmessage = (e) => {
    let m;
    try { m = JSON.parse(e.data); } catch (err) { return; }
    handle(m);
  };
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  const jitter = Math.random() * 400;
  const delay = reconnectDelay + jitter;
  reconnectDelay = Math.min(reconnectDelay * 2, 5000);
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, delay);
  refreshConn();
}

/* ---------- 指令：送出／緩衝／補發 ---------- */
function send(obj) {
  return new Promise((resolve, reject) => {
    const id = ++msgId;
    obj.id = id;
    const entry = {
      resolve, reject,
      timer: setTimeout(() => {
        pending.delete(id);
        removeFromBuffer(id);
        reject(new Error('timeout'));
      }, 25000)
    };
    pending.set(id, entry);
    if (wsOpen) {
      ws.send(JSON.stringify(obj));
    } else {
      buffer.push({ id, obj });
      if (buffer.length > 10) {
        const old = buffer.shift();
        const p = pending.get(old.id);
        if (p) { clearTimeout(p.timer); pending.delete(old.id); p.reject(new Error('offline')); }
      }
      toast('連線中斷，指令將在恢復後自動送出', 'err');
    }
  });
}

function flushBuffer() {
  if (!buffer.length) return;
  const n = buffer.length;
  while (buffer.length) {
    const item = buffer.shift();
    if (wsOpen && ws.readyState === 1) ws.send(JSON.stringify(item.obj));
  }
  toast('已恢復連線，補送 ' + n + ' 條指令', 'ok');
}

function removeFromBuffer(id) {
  for (let i = buffer.length - 1; i >= 0; i--) {
    if (buffer[i].id === id) buffer.splice(i, 1);
  }
}

function failAllPending(msg) {
  for (const [, p] of pending) {
    clearTimeout(p.timer);
    try { p.reject(new Error(msg || '離線')); } catch (e) { /* 忽略 */ }
  }
  pending.clear();
  buffer.length = 0;
}

/* ---------- 訊息處理 ---------- */
function handle(m) {
  if (m.t === 'status') {
    const json = JSON.stringify(m);
    if (json === lastStatusJson) return;   // 無變化不重渲染
    lastStatusJson = json;
    lastStatus = {
      online: m.online !== false,
      free: m.free, total: m.total || 8,
      busy: !!m.busy, cur: m.cur || 1, codes: m.codes || []
    };
    applyOnline(lastStatus.online);
    renderStatus();
    if (!lastStatus.busy) flushIdle();
  } else if (m.t === 'presence') {
    if (lastStatus.online !== !!m.online) {
      lastStatus.online = !!m.online;
      lastStatusJson = '';
      applyOnline(lastStatus.online);
      renderStatus();
      if (m.online) toast('櫃子已上線', 'ok');
    }
  } else if (m.t === 'cmdlog') {
    appendCmdLog(m);
  } else if (m.t === 'result') {
    const p = pending.get(m.id);
    if (p) {
      clearTimeout(p.timer);
      pending.delete(m.id);
      p.resolve(m);
    }
  }
}

/* ---------- 即時指令監控 ---------- */
let cmdLogCount = 0;
function appendCmdLog(m) {
  const box = $('cmdLog');
  if (!box) return;
  const empty = box.querySelector('.empty');
  if (empty) empty.remove();
  const now = new Date();
  const ts = String(now.getHours()).padStart(2, '0') + ':' +
             String(now.getMinutes()).padStart(2, '0') + ':' +
             String(now.getSeconds()).padStart(2, '0');
  const line = document.createElement('div');
  line.className = 'l';
  if (m.dir === 'tx') {
    line.innerHTML = '<span class="t">' + ts + '</span><span class="tx">🔀 ESP32 收到：' + escapeHtml(String(m.cmd || '')) + '</span>';
  } else {
    const okCls = m.ok ? 'ok' : 'err';
    const icon = m.ok ? '🟢' : '🔴';
    let txt = icon + ' ESP32 回執：' + escapeHtml(String(m.msg || ''));
    if (m.ok && m.slot) txt += '（第 ' + m.slot + ' 格）';
    if (m.ok && m.code) txt += ' 碼：' + escapeHtml(String(m.code));
    line.innerHTML = '<span class="t">' + ts + '</span><span class="rx ' + okCls + '">' + txt + '</span>';
  }
  box.appendChild(line);
  cmdLogCount++;
  while (box.children.length > 60) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}
function escapeHtml(s) {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

/* ---------- 渲染 ---------- */
function renderStatus() {
  $('free').textContent = (effectiveOnline && lastStatus.online) ? lastStatus.free : '–';
  $('slotInfo').textContent = '取\u50d8口對正：第 ' + lastStatus.cur + ' 格';
  $('busy').classList.toggle('hidden', !lastStatus.busy);
  $('offlineWarn').classList.toggle('hidden', !(wsOpen && !(effectiveOnline && lastStatus.online)));
  $('bs').disabled = !(effectiveOnline && lastStatus.online) || lastStatus.busy;
  renderSlotTable();
  refreshConn();
}

function renderSlotTable() {
  const t = $('slotTable');
  let html = '<tr><th>格位</th><th>取\u50d8碼</th></tr>';
  const codes = lastStatus.codes || [];
  for (let i = 0; i < (lastStatus.total || 8); i++) {
    const c = codes[i];
    html += '<tr><td>第 ' + (i + 1) + ' 格' + (i === (lastStatus.cur - 1) ? '（對正）' : '') + '</td><td>' +
      (c && c !== '' ? c : '空') + '</td></tr>';
  }
  t.innerHTML = html;
}

/* ---------- 取僘碼鍵盤 ---------- */
function renderBoxes() {
  const boxes = $('codeBoxes').children;
  for (let i = 0; i < 4; i++) {
    boxes[i].textContent = takeCode[i] || '';
    boxes[i].classList.toggle('filled', i < takeCode.length);
  }
}
function renderSavedCode(code) {
  const card = $('savedCode');
  if (!card) return;
  const valid = /^\d{4}$/.test(code || '');
  card.classList.toggle('hidden', !valid);
  if (valid) $('savedCodeValue').textContent = code;
}
function useSavedCode() {
  let saved = '';
  try { saved = localStorage.getItem('umbrellaCode') || ''; } catch (e) { /* 忽略 */ }
  if (!/^\d{4}$/.test(saved)) return;
  takeCode = saved;
  renderBoxes();
  $('takeCard').scrollIntoView({ behavior: 'smooth', block: 'start' });
  toast('已填入本機保留的取傘碼', 'ok');
  buzz(BUZZ.tap);
}
function shakeBoxes() {
  const el = $('codeBoxes');
  el.classList.remove('shake');
  void el.offsetWidth;
  el.classList.add('shake');
}
function keypadPress(k) {
  buzz(BUZZ.tap);
  if (k === 'del') {
    takeCode = takeCode.slice(0, -1);
  } else if (k === 'ok') {
    submitTake();
    return;
  } else if (takeCode.length < 4) {
    takeCode += k;
  }
  renderBoxes();
}

/* ---------- 動作 ---------- */
function setMsg(t, cls) {
  const el = $('m');
  el.textContent = t || '';
  el.className = 'msg' + (cls ? ' ' + cls : '');
}

function hideResultCard() { $('resultCard').classList.add('hidden'); }

async function store() {
  hideResultCard();
  setMsg('請稍候…');
  buzz(BUZZ.tap);
  try {
    const r = await send({ t: 'store' });
    if (!r.ok) { setMsg(r.msg || '存\u50d8失敗', 'err'); buzz(BUZZ.err); return; }
    try { localStorage.setItem('umbrellaCode', r.code); } catch (e) { /* 忽略 */ }
    renderSavedCode(r.code);
    $('rcSlot').textContent = '已存入第 ' + r.slot + ' 格';
    $('rcCode').textContent = r.code;
    $('resultCard').classList.remove('hidden');
    $('resultCard').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    setMsg('');
    buzz(BUZZ.ok);
    await waitIdle();
    setMsg('✅ 完成，門已關閉', 'ok');
  } catch (e) {
    setMsg(e.message === 'timeout' ? '櫃子沒有回應，請重新整理再試' : '連線中斷，請稍後再試', 'err');
    buzz(BUZZ.err);
  }
}

async function submitTake() {
  if (takeCode.length !== 4) {
    shakeBoxes();
    buzz(BUZZ.err);
    toast('請輸入 4 位取\u50d8碼', 'err');
    return;
  }
  hideResultCard();
  setMsg('請稍候…');
  buzz(BUZZ.tap);
  try {
    const r = await send({ t: 'take', code: takeCode });
    if (!r.ok) {
      setMsg(r.msg || '取\u50d8失敗', 'err');
      shakeBoxes();
      buzz(BUZZ.err);
      return;
    }
    try { localStorage.removeItem('umbrellaCode'); } catch (e) { /* 忽略 */ }
    renderSavedCode('');
    setMsg('第 ' + r.slot + ' 格開門中，請取走雨\u50d8');
    buzz(BUZZ.ok);
    await waitIdle();
    setMsg('✅ 完成，歡迎再用', 'ok');
    takeCode = '';
    renderBoxes();
  } catch (e) {
    setMsg(e.message === 'timeout' ? '櫃子沒有回應，請重新整理再試' : '連線中斷，請稍後再試', 'err');
    buzz(BUZZ.err);
  }
}

/* ---------- 管理 ---------- */
function adminKey() { return $('ak').value.trim() || localStorage.getItem('umbrellaAdminKey') || ''; }

async function admin(act, n) {
  try {
    const r = await send({ t: 'admin', key: adminKey(), act, n: n || 0 });
    if (r.ok) {
      try { localStorage.setItem('umbrellaAdminKey', adminKey()); } catch (e) { /* 忽略 */ }
      buzz(BUZZ.ok);
    } else {
      buzz(BUZZ.err);
    }
    return r;
  } catch (e) {
    buzz(BUZZ.err);
    return { ok: false, msg: '連線中斷' };
  }
}

/* ---------- 等待櫃子空閒 ---------- */
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

/* ---------- 斷線時的唯讀輪詢 ---------- */
let pollTimer = null;
function startStatusPoll() {
  if (pollTimer) return;
  const poll = async () => {
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      const s = await r.json();
      if (wsOpen) { stopStatusPoll(); return; }   // WS 已恢復，交給 WS
      lastStatus = { online: !!s.online, free: s.free, total: s.total || 8, busy: !!s.busy, cur: s.cur || 1, codes: s.codes || [] };
      lastStatusJson = '';
      renderStatus();
      setPill(lastStatus.online ? 'on' : 'off', lastStatus.online ? '櫃子在線（只讀）' : '櫃子離線');
    } catch (e) { setPill('wait', '連接中…'); }
  };
  poll();
  pollTimer = setInterval(poll, 5000);
}
function stopStatusPoll() { if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } }

/* ---------- 複製取僘碼 ---------- */
async function copyCode() {
  const code = $('rcCode').textContent;
  try {
    await navigator.clipboard.writeText(code);
    toast('已複製：' + code, 'ok');
  } catch (e) {
    const ta = document.createElement('textarea');
    ta.value = code;
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); toast('已複製：' + code, 'ok'); }
    catch (e2) { toast('複製失敗，請手動抄下', 'err'); }
    ta.remove();
  }
}
async function shareCode() {
  const code = $('rcCode').textContent;
  if (!code) return;
  try {
    if (navigator.share) {
      await navigator.share({ title: '雨傘櫃取傘碼', text: '我的雨傘櫃取傘碼是：' + code });
      toast('已開啟分享', 'ok');
    } else {
      await copyCode();
      toast('此裝置不支援分享，已改為複製', 'ok');
    }
  } catch (e) {
    if (e.name !== 'AbortError') toast('分享未完成，取傘碼仍在本機保留', 'err');
  }
}

/* ---------- 初始化 ---------- */
window.addEventListener('DOMContentLoaded', () => {
  let saved = '';
  try { saved = localStorage.getItem('umbrellaCode') || ''; } catch (e) { /* 忽略 */ }
  if (saved) {
    setMsg('上次的取傘碼已保存在本機，可按「一鍵填入」');
    renderSavedCode(saved);
  }

  try { $('ak').value = localStorage.getItem('umbrellaAdminKey') || ''; } catch (e) { /* 忽略 */ }

  $('bs').addEventListener('click', store);
  $('keypad').addEventListener('click', (e) => {
    const k = e.target && e.target.dataset ? e.target.dataset.k : null;
    if (k) keypadPress(k);
  });
  $('rcCopy').addEventListener('click', copyCode);
  $('rcShare').addEventListener('click', shareCode);
  $('useSavedCode').addEventListener('click', useSavedCode);
  $('rcDone').addEventListener('click', () => { hideResultCard(); setMsg(''); });
  $('cmdClear').addEventListener('click', () => {
    const box = $('cmdLog');
    box.innerHTML = '<div class="empty">已清空，等待指令…</div>';
  });

  $('aopen').addEventListener('click', async () => { const r = await admin('open'); setMsg(r.ok ? '門已開' : (r.msg || '失敗'), r.ok ? 'ok' : 'err'); });
  $('aclose').addEventListener('click', async () => { const r = await admin('close'); setMsg(r.ok ? '門已關' : (r.msg || '失敗'), r.ok ? 'ok' : 'err'); });
  $('aclear').addEventListener('click', async () => { const r = await admin('clear'); setMsg(r.ok ? '已清空全部格位' : (r.msg || '失敗'), r.ok ? 'ok' : 'err'); });

  for (let i = 1; i <= 8; i++) {
    const b1 = document.createElement('button');
    b1.className = 'mini';
    b1.textContent = i;
    b1.addEventListener('click', async () => { const r = await admin('home', i); setMsg(r.ok ? ('已對位：第 ' + i + ' 格') : (r.msg || '失敗'), r.ok ? 'ok' : 'err'); });
    $('homeGrid').appendChild(b1);

    const b2 = document.createElement('button');
    b2.className = 'mini';
    b2.textContent = i;
    b2.addEventListener('click', async () => { const r = await admin('go', i); setMsg(r.ok ? ('已轉到第 ' + i + ' 格') : (r.msg || '失敗'), r.ok ? 'ok' : 'err'); });
    $('goGrid').appendChild(b2);
  }

  /* 實體鍵盤支持（桌面／平板）；聚焦在按鈕上時不攔 Enter，避免雙重觸發 */
  document.addEventListener('keydown', (e) => {
    const tag = e.target && e.target.tagName;
    if (tag === 'INPUT' || tag === 'TEXTAREA') return;
    if (/^[0-9]$/.test(e.key)) { e.preventDefault(); keypadPress(e.key); }
    else if (e.key === 'Backspace') { e.preventDefault(); keypadPress('del'); }
    else if (e.key === 'Enter' && tag !== 'BUTTON') { keypadPress('ok'); }
  });

  /* 回到前景時立即重連（不等退避） */
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !wsOpen) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
      reconnectDelay = 1000;
      connect();
    }
  });

  /* 全局錯誤邊界：不讓頁面悄無聲息地壞掉 */
  window.addEventListener('error', (e) => {
    toast('頁面出現異常，請重新整理', 'err');
    console.error('[app]', e.message);
  });

  renderStatus();
  renderBoxes();
  connect();

  /* Service Worker（PWA 離線殼，失敗不影響功能） */
  if (navigator.serviceWorker) {
    navigator.serviceWorker.register('sw.js').catch(() => { /* 忽略 */ });
  }
});
