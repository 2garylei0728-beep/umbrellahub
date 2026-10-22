/*
 * 智能旋轉式雨傘寄存櫃 — 在線控制中繼伺服器 v2
 * 瀏覽器 ── WebSocket/HTTPS ──► 本伺服器 ── WebSocket ──► ESP32
 *
 * ESP32 的既有協定是純文字命令（store、take 1234、open…），
 * 設備回覆是 JSON 且不帶 request id；伺服器因此只允許一個設備命令飛行中，
 * 以避免多用戶同時操作時回覆錯配。
 */
'use strict';

const path = require('path');
const http = require('http');
const express = require('express');
const compression = require('compression');
const { WebSocketServer } = require('ws');

const PORT = parseInt(process.env.PORT || '3000', 10);
const DEVICE_SECRET = process.env.DEVICE_SECRET || 'change-me-device-secret';
const ADMIN_KEY = process.env.ADMIN_KEY || '8888';
const DEVICE_TIMEOUT_MS = parseInt(process.env.DEVICE_TIMEOUT_MS || '90000', 10);  // 僵屍判定 90s（邊緣網絡更寬容，防誤殺；ESP32 每 1.5s 推 status 正常遠低於此）
const CMD_GRACE_MS = parseInt(process.env.CMD_GRACE_MS || '8000', 10);
const MAX_QUEUE = 30;
const MAX_INFLIGHT_PER_USER = 5;
const RESULT_WAIT_MS = 60000;
const SLOT_COUNT = 8;

function log(...args) {
  const stamp = new Date().toISOString().replace('T', ' ').slice(0, 19);
  console.log(stamp, ...args);
}

const app = express();
app.disable('x-powered-by');

/*
 * X-Frame-Options intentionally omitted: Manus Preview is embedded in a
 * cross-site iframe. CSP also deliberately has no frame-ancestors directive.
 */
app.use((req, res, next) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('Referrer-Policy', 'same-origin');
  res.setHeader(
    'Content-Security-Policy',
    "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; script-src 'self'"
  );
  next();
});

app.use(compression());
app.use(express.static(path.join(__dirname, 'public'), {
  extensions: ['html'],
  setHeaders: (res, filePath) => {
    const base = path.basename(filePath);
    if (base === 'index.html' || base === 'sw.js' || base === 'manifest.webmanifest') {
      res.setHeader('Cache-Control', 'no-cache');
    } else {
      res.setHeader('Cache-Control', 'public, max-age=3600');
    }
  }
}));

/* ---------- 狀態 ---------- */
let device = null;
let deviceLastSeen = 0;
const users = new Set();
let lastStatus = { online: false, free: SLOT_COUNT, total: SLOT_COUNT, busy: false, cur: 1, codes: [] };
let lastBroadcastJson = '';

/* 每個 entry 對應一個瀏覽器請求；設備回覆沒有 id，由 commandInFlight 配對。 */
const commandMap = new Map();
const commandQueue = [];
let commandInFlight = null;
let commandSequence = 0;

/* ---------- 通訊工具 ---------- */
function safeSend(ws, object) {
  if (!ws || ws.readyState !== 1) return false;
  try {
    ws.send(JSON.stringify(object));
    return true;
  } catch (_) {
    return false;
  }
}

function broadcast(object) {
  const text = JSON.stringify(object);
  for (const ws of users) {
    if (ws.readyState !== 1) continue;
    try { ws.send(text); } catch (_) { /* 由 close 事件清理 */ }
  }
}

function sendToDevice(command) {
  if (!device || device.readyState !== 1) return false;
  try {
    device.send(command);
    return true;
  } catch (_) {
    return false;
  }
}

function setDeviceOnline(online) {
  lastStatus.online = online;
  if (!online) {
    lastStatus.free = lastStatus.total;
    lastStatus.busy = false;
  }
  broadcast({ t: 'presence', online });
}

function sendCommandResult(entry, fields) {
  safeSend(entry.ws, { t: 'result', id: entry.clientId, ...fields });
  /* 監控廣播：讓網頁「即時指令監控」面板能看到設備回執（普通用戶忽略此訊息） */
  broadcast({ t: 'cmdlog', dir: 'rx', ok: fields && fields.ok, msg: (fields && fields.msg) || '', slot: fields && fields.slot, code: fields && fields.code });
}

function removeFromQueue(entry) {
  const index = commandQueue.indexOf(entry);
  if (index >= 0) commandQueue.splice(index, 1);
}

function failEntry(entry, message) {
  if (!entry) return;
  if (entry.timer) clearTimeout(entry.timer);
  entry.timer = null;
  if (commandInFlight === entry) commandInFlight = null;
  commandMap.delete(entry.serverId);
  removeFromQueue(entry);
  if (!entry.resultSent) sendCommandResult(entry, { ok: false, msg: message });
}

function completeEntry(entry, fields) {
  if (!entry) return;
  if (entry.timer) clearTimeout(entry.timer);
  entry.timer = null;
  if (commandInFlight === entry) commandInFlight = null;
  commandMap.delete(entry.serverId);
  removeFromQueue(entry);
  if (!entry.resultSent && fields) {
    sendCommandResult(entry, fields);
    entry.resultSent = true;
  }
  pumpQueue();
}

function userInflightCount(ws) {
  let count = 0;
  for (const entry of commandMap.values()) {
    if (entry.ws === ws) count++;
  }
  return count;
}

function scheduleEntryExpiry(entry, delay, message) {
  if (entry.timer) clearTimeout(entry.timer);
  entry.timer = setTimeout(() => {
    if (!commandMap.has(entry.serverId)) return;
    failEntry(entry, message);
    pumpQueue();
  }, delay);
}

function commandText(request) {
  if (request.t === 'store') return 'store';
  if (request.t === 'take') return 'take ' + request.code;
  if (request.t !== 'admin') return null;
  if (['open', 'close', 'clear'].includes(request.act)) return request.act;
  if (request.act === 'home' || request.act === 'go') {
    const n = Number(request.n);
    if (Number.isInteger(n) && n >= 1 && n <= SLOT_COUNT) return request.act + ' ' + n;
  }
  return null;
}

/* 只把一個純文字命令送給 ESP32；上一個回覆後才送下一個。 */
function pumpQueue() {
  if (commandInFlight || !device || device.readyState !== 1) return;
  while (commandQueue.length) {
    const entry = commandQueue.shift();
    if (!commandMap.has(entry.serverId)) continue;
    if (!entry.ws || entry.ws.readyState !== 1) {
      commandMap.delete(entry.serverId);
      if (entry.timer) clearTimeout(entry.timer);
      continue;
    }

    commandInFlight = entry;
    entry.delivered = true;
    if (!sendToDevice(entry.command)) {
      commandInFlight = null;
      scheduleEntryExpiry(entry, CMD_GRACE_MS, '傳送失敗，請稍後再試');
      commandQueue.unshift(entry);
      return;
    }
    /* 監控廣播：ESP32 實際收到的純文字命令 */
    broadcast({ t: 'cmdlog', dir: 'tx', cmd: entry.command });
    scheduleEntryExpiry(entry, RESULT_WAIT_MS, '櫃子沒有回應，請重新整理再試');
    return;
  }
}

function submitCommand(ws, request) {
  const clientId = request.id;
  if (!Number.isInteger(clientId)) return;

  if (userInflightCount(ws) >= MAX_INFLIGHT_PER_USER) {
    safeSend(ws, { t: 'result', id: clientId, ok: false, msg: '操作太頻繁，請稍候' });
    return;
  }
  if (commandMap.size >= MAX_QUEUE) {
    safeSend(ws, { t: 'result', id: clientId, ok: false, msg: '系統忙碌中，請稍後再試' });
    return;
  }

  const command = commandText(request);
  if (!command) {
    safeSend(ws, { t: 'result', id: clientId, ok: false, msg: '不支援的操作' });
    return;
  }

  const entry = {
    serverId: ++commandSequence,
    clientId,
    ws,
    command,
    delivered: false,
    resultSent: false,
    sawBusy: false,
    expectedSlot: request.t === 'take'
      ? (lastStatus.codes || []).indexOf(request.code) + 1
      : null,
    timer: null
  };
  commandMap.set(entry.serverId, entry);
  commandQueue.push(entry);

  if (device && device.readyState === 1) {
    scheduleEntryExpiry(entry, RESULT_WAIT_MS, '櫃子沒有回應，請重新整理再試');
  } else {
    scheduleEntryExpiry(entry, CMD_GRACE_MS, '櫃子暫時離線，請稍後再試');
    log('[cmd] 設備離線，指令進入寬限隊列（', commandQueue.length, '筆）');
  }
  pumpQueue();
}

function cleanupUserCommands(ws) {
  for (const entry of Array.from(commandMap.values())) {
    if (entry.ws === ws) failEntry(entry, '用戶已離線，操作已取消');
  }
  pumpQueue();
}

/* ---------- HTTP ---------- */
app.get('/healthz', (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  try {
    res.json({
      ok: true,
      deviceOnline: !!device,
      deviceSilentSec: device ? Math.round((Date.now() - deviceLastSeen) / 1000) : null,
      users: users.size,
      queued: commandQueue.length,
      commandInFlight: !!commandInFlight,
      uptime: Math.round(process.uptime())
    });
  } catch (_) {
    res.status(500).json({ ok: false });
  }
});

app.get('/api/status', (req, res) => {
  res.setHeader('Cache-Control', 'no-store');
  res.json({ ...lastStatus, online: !!device });
});

const server = http.createServer(app);
server.keepAliveTimeout = 65000;
server.headersTimeout = 70000;
const wss = new WebSocketServer({
  server,
  path: '/ws',
  maxPayload: 16 * 1024,
  perMessageDeflate: false
});

/* ---------- WebSocket ---------- */
wss.on('connection', (ws, req) => {
  let role = 'user';
  try {
    role = (new URL(req.url, 'http://localhost').searchParams.get('role') || 'user').toLowerCase();
  } catch (_) { /* 使用 user 預設 */ }

  ws.isAlive = true;
  ws.lastSeen = Date.now();
  ws.on('pong', () => { ws.isAlive = true; ws.lastSeen = Date.now(); });
  ws.on('error', (error) => log('[ws] 連線錯誤：', error.message));
  if (ws._socket) ws._socket.setNoDelay(true);

  if (role === 'device') {
    let secret = '';
    try { secret = new URL(req.url, 'http://localhost').searchParams.get('secret') || ''; } catch (_) { /* 空密鑰 */ }
    if (secret !== DEVICE_SECRET) {
      safeSend(ws, { t: 'result', ok: false, msg: '設備密鑰錯誤' });
      try { ws.close(4001, 'bad secret'); } catch (_) { /* 忽略 */ }
      log('[device] 拒絕連線：密鑰錯誤');
      return;
    }

    if (device && device !== ws && device.readyState === 1) {
      log('[device] 新裝置連入，踢掉舊連線');
      if (commandInFlight) failEntry(commandInFlight, '設備重新連線，原操作已取消');
      try { device.close(4002, 'replaced'); } catch (_) { /* 忽略 */ }
    }
    device = ws;
    deviceLastSeen = Date.now();
    log('[device] 已連線');
    setDeviceOnline(true);
    /* 韌體會忽略 JSON hello；不把 ADMIN_KEY 傳給設備。 */
    safeSend(ws, { t: 'hello', fw: 'server-v2' });
    pumpQueue();

    ws.on('message', (raw) => {
      deviceLastSeen = Date.now();
      ws.lastSeen = Date.now();   // heartbeat 保活計時
      let message;
      try { message = JSON.parse(raw.toString()); } catch (_) { return; }

      if (message.t === 'status') {
        lastStatus = {
          online: true,
          free: Number.isFinite(Number(message.free)) ? Number(message.free) : 0,
          total: Number.isFinite(Number(message.total)) && Number(message.total) > 0 ? Number(message.total) : SLOT_COUNT,
          busy: !!message.busy,
          cur: Number.isFinite(Number(message.cur)) ? Number(message.cur) : 1,
          codes: Array.isArray(message.codes) ? message.codes.map(String) : []
        };
        const active = commandInFlight;
        if (active && (active.command === 'store' || active.command.startsWith('take ')) && lastStatus.busy) {
          active.sawBusy = true;
        }
        if (active && active.sawBusy && !lastStatus.busy) {
          if (active.command === 'store' && active.resultSent) {
            completeEntry(active);
          } else if (active.command.startsWith('take ') && !active.resultSent) {
            /* 既有 v4.2 韌體的成功取傘流程只推送 busy 狀態，不推 result。 */
            completeEntry(active, { ok: true, msg: '', slot: active.expectedSlot || lastStatus.cur });
          }
        }
        const json = JSON.stringify(lastStatus);
        if (json !== lastBroadcastJson) {
          lastBroadcastJson = json;
          broadcast({ t: 'status', ...lastStatus });
        }
      } else if (message.t === 'result') {
        const entry = commandInFlight;
        if (!entry) {
          log('[device] 收到沒有對應命令的結果');
          return;
        }
        const fields = {
          ok: !!message.ok,
          msg: message.msg || '',
          slot: message.slot,
          code: message.code
        };
        if (entry.command === 'store' && message.ok) {
          /* 存傘回覆先到，等待後續 busy=false 才允許下一個命令。 */
          sendCommandResult(entry, fields);
          entry.resultSent = true;
          if (entry.sawBusy && !lastStatus.busy) completeEntry(entry);
        } else {
          completeEntry(entry, fields);
        }
      }
    });

    ws.on('close', () => {
      if (device !== ws) return;
      device = null;
      log('[device] 離線');
      if (commandInFlight) failEntry(commandInFlight, '櫃子連線中斷，請稍後再試');
      setDeviceOnline(false);
      pumpQueue();
    });
    return;
  }

  users.add(ws);
  log('[user] 連入，共', users.size, '人');
  safeSend(ws, { t: 'status', ...lastStatus, online: !!device });

  ws.on('message', (raw) => {
    ws.lastSeen = Date.now();   // heartbeat 保活計時
    let message;
    try { message = JSON.parse(raw.toString()); } catch (_) { return; }
    const id = message.id;
    if (!Number.isInteger(id)) return;

    if (message.t === 'admin_auth') {
      const ok = String(message.key || '') === ADMIN_KEY;
      safeSend(ws, { t: 'result', id, ok, msg: ok ? '管理密碼正確' : '管理密碼錯誤' });
      return;
    }

    if (message.t === 'store') {
      submitCommand(ws, { t: 'store', id });
      return;
    }
    if (message.t === 'take') {
      const code = String(message.code || '').trim();
      if (!/^\d{4}$/.test(code)) {
        safeSend(ws, { t: 'result', id, ok: false, msg: '請輸入 4 位取\u5098碼' });
        return;
      }
      submitCommand(ws, { t: 'take', code, id });
      return;
    }
    if (message.t === 'admin') {
      if (String(message.key || '') !== ADMIN_KEY) {
        safeSend(ws, { t: 'result', id, ok: false, msg: '管理密碼錯誤' });
        return;
      }
      submitCommand(ws, {
        t: 'admin',
        act: String(message.act || ''),
        n: Number(message.n) || 0,
        id
      });
    }
  });

  ws.on('close', () => {
    users.delete(ws);
    cleanupUserCommands(ws);
    log('[user] 離線，剩餘', users.size, '人');
  });
});

/* ---------- 連線與設備看守 ---------- */
const heartbeat = setInterval(() => {
  const now = Date.now();
  for (const ws of wss.clients) {
    /* 以「最後訊息時間」判定（與 watchdog 同源）：ESP32 每 1.5s 推 status 即保活；
       不再因 ping 無 pong 回應終止——arduinoWebSockets 等嵌入式客戶端不一定回 pong，
       舊邏輯會每 60 秒誤殺一次，造成網站「時連時斷」（2026-10-22 實測根因） */
    if (now - (ws.lastSeen || 0) > DEVICE_TIMEOUT_MS) {
      log('[ws] 心跳無回應（超過 ' + DEVICE_TIMEOUT_MS + 'ms 無訊息），終止連線');
      try { ws.terminate(); } catch (_) { /* 忽略 */ }
      continue;
    }
    try { ws.ping(); } catch (_) { /* 忽略 */ }   // 仍然 ping（保持中間設備活躍），但不因無 pong 終止
  }
}, 30000);

const watchdog = setInterval(() => {
  if (device && Date.now() - deviceLastSeen > DEVICE_TIMEOUT_MS) {
    log('[device] ', Math.round((Date.now() - deviceLastSeen) / 1000), '秒無訊息，判定僵屍連線，終止');
    try { device.terminate(); } catch (_) { /* 忽略 */ }
  }
}, 5000);

process.on('uncaughtException', (error) => log('[fatal] uncaughtException（服務續跑）：', error && error.stack ? error.stack : error));
process.on('unhandledRejection', (error) => log('[fatal] unhandledRejection（服務續跑）：', error));

server.listen(PORT, '0.0.0.0', () => {
  log('==============================================');
  log(' 智能雨\u5098寄存櫃 — 在線控制伺服器 v2 已啟動 :' + PORT);
  log(' 本機測試：http://localhost:' + PORT);
  log(' 設備密鑰：' + (process.env.DEVICE_SECRET ? '(已設定)' : '(預設值，請務必修改)'));
  log(' 寬限隊列：' + CMD_GRACE_MS + 'ms｜僵屍判定：' + DEVICE_TIMEOUT_MS + 'ms');
  log('==============================================');
});

function shutdown(signal) {
  log('收到 ' + signal + '，正在優雅關閉…');
  clearInterval(heartbeat);
  clearInterval(watchdog);
  for (const entry of Array.from(commandMap.values())) failEntry(entry, '伺服器正在重啟，請稍後再試');
  for (const ws of wss.clients) {
    try { ws.close(1001, 'server shutdown'); } catch (_) { /* 忽略 */ }
  }
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 10000).unref();
}
process.once('SIGTERM', () => shutdown('SIGTERM'));
process.once('SIGINT', () => shutdown('SIGINT'));
