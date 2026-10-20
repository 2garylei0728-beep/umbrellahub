/*
 * 智能旋轉式雨傘寄存櫃 — 在線控制中繼伺服器
 * 架構：手機網站 ──WebSocket──► 本伺服器 ──WebSocket──► ESP32（連校園 WiFi）
 * 用戶只需打開網站網址，不需連接設備熱點。
 *
 * 環境變數：
 *   PORT           服務埠（平台通常自動注入，預設 3000）
 *   DEVICE_SECRET  設備密鑰：ESP32 固件 SERVER_URL 裡的 secret 必須與它相同
 *   ADMIN_KEY      管理密碼：網站管理操作（開門/清空/對位等）需要
 */
'use strict';

const path = require('path');
const http = require('http');
const express = require('express');
const { WebSocketServer } = require('ws');

const PORT = parseInt(process.env.PORT || '3000', 10);
const DEVICE_SECRET = process.env.DEVICE_SECRET || 'change-me-device-secret';
const ADMIN_KEY = process.env.ADMIN_KEY || '8888';

const app = express();
app.disable('x-powered-by');
app.use(express.static(path.join(__dirname, 'public'), { extensions: ['html'] }));

app.get('/healthz', (req, res) => {
  res.json({
    ok: true,
    deviceOnline: !!device,
    users: users.size,
    commandInFlight: !!commandInFlight,
    queuedCommands: commandQueue.length,
    deviceBlocked,
    uptime: Math.round(process.uptime())
  });
});

app.get('/api/status', (req, res) => {
  res.set('Cache-Control', 'no-store');
  res.json(Object.assign({}, lastStatus, { online: !!device }));
});

const server = http.createServer(app);
server.keepAliveTimeout = 65000;
server.headersTimeout = 70000;
const wss = new WebSocketServer({ server, path: '/ws', maxPayload: 16 * 1024, perMessageDeflate: false });

/* ---------- 狀態 ---------- */
let device = null;                 // 設備（ESP32）連線，單設備設計
const users = new Set();           // 所有手機/電腦瀏覽器
let lastStatus = { online: false, free: 8, total: 8, busy: false, cur: 1, codes: [] };
const COMMAND_TIMEOUT_MS = 45000;  // 覆蓋旋轉、開門等待與關門的最長正常操作時間
const MAX_COMMAND_QUEUE = 16;      // 防止斷線重連或重複點擊造成無限堆積
let commandInFlight = null;         // ESP32 韌體不回傳 request id，由伺服器維持單一飛行中的命令
let deviceBlocked = false;          // 逾時後等待設備重新連線，避免晚到回應誤配
const commandQueue = [];

/* ---------- 工具 ---------- */
function safeSend(ws, obj) {
  if (ws && ws.readyState === 1) {
    try { ws.send(JSON.stringify(obj)); } catch (_) { /* 忽略 */ }
  }
}

function broadcast(obj, exceptWs) {
  const s = JSON.stringify(obj);
  for (const u of users) {
    if (u !== exceptWs && u.readyState === 1) {
      try { u.send(s); } catch (_) { /* 忽略 */ }
    }
  }
}

// ESP32 韌體的既有協定是純文字命令，不是 JSON。
function sendToDevice(command) {
  if (!device || device.readyState !== 1) return false;
  try { device.send(command); return true; } catch (_) { return false; }
}

function sendResult(ws, id, fields) {
  safeSend(ws, { t: 'result', id, ...fields });
}

function failCommand(command, msg) {
  if (command) sendResult(command.ws, command.id, { ok: false, msg });
}

function discardDeviceCommands(msg) {
  if (commandInFlight) {
    clearTimeout(commandInFlight.timer);
    failCommand(commandInFlight, msg);
  }
  commandInFlight = null;
  while (commandQueue.length) failCommand(commandQueue.shift(), msg);
}

function pumpDeviceQueue() {
  if (commandInFlight || deviceBlocked || !device || device.readyState !== 1) return;
  while (commandQueue.length) {
    const next = commandQueue.shift();
    if (!next.ws || next.ws.readyState !== 1) continue;
    commandInFlight = next;
    next.timer = setTimeout(() => {
      if (commandInFlight !== next) return;
      commandInFlight = null;
      failCommand(next, '櫃子回應逾時，請稍後查看狀態');
      deviceBlocked = true;
      discardDeviceCommands('設備回應逾時，請重新連線後再試');
    }, COMMAND_TIMEOUT_MS);
    if (!sendToDevice(next.command)) {
      clearTimeout(next.timer);
      commandInFlight = null;
      failCommand(next, '傳送失敗，請重試');
      continue;
    }
    return;
  }
}

function enqueueDeviceCommand(ws, id, command) {
  if (!device || device.readyState !== 1) {
    sendResult(ws, id, { ok: false, msg: '櫃子暫時離線，請稍後再試' });
    return;
  }
  if (deviceBlocked) {
    sendResult(ws, id, { ok: false, msg: '設備需要重新連線，請稍後再試' });
    return;
  }
  if (commandQueue.length >= MAX_COMMAND_QUEUE) {
    sendResult(ws, id, { ok: false, msg: '操作太多，請稍後再試' });
    return;
  }
  commandQueue.push({ ws, id, command, timer: null });
  pumpDeviceQueue();
}

function cancelQueuedCommandsFor(ws) {
  for (let i = commandQueue.length - 1; i >= 0; i--) {
    if (commandQueue[i].ws === ws) commandQueue.splice(i, 1);
  }
}

function setDeviceOnline(online) {
  lastStatus.online = online;
  if (!online) { lastStatus.free = lastStatus.total; lastStatus.busy = false; }
  broadcast({ t: 'presence', online });
}

/* ---------- WebSocket ---------- */
wss.on('connection', (ws, req) => {
  let role = 'user';
  try {
    const u = new URL(req.url, 'http://localhost');
    role = (u.searchParams.get('role') || 'user').toLowerCase();
  } catch (_) { /* 用預設 */ }

  ws.isAlive = true;
  ws.on('pong', () => { ws.isAlive = true; });
  ws.on('error', (err) => console.warn('[ws] 連線錯誤：' + err.message));
  if (ws._socket) ws._socket.setNoDelay(true);

  if (role === 'device') {
    /* --- ESP32 裝置端 --- */
    const secret = (() => {
      try { return new URL(req.url, 'http://localhost').searchParams.get('secret') || ''; } catch (_) { return ''; }
    })();
    if (secret !== DEVICE_SECRET) {
      safeSend(ws, { t: 'result', ok: false, msg: '設備密鑰錯誤' });
      try { ws.close(4001, 'bad secret'); } catch (_) { /* 忽略 */ }
      console.log('[device] 拒絕連線：密鑰錯誤');
      return;
    }
    if (device && device !== ws && device.readyState === 1) {
      console.log('[device] 新裝置連入，踢掉舊連線');
      discardDeviceCommands('設備重新連線，原操作已取消');
      try { device.close(4002, 'replaced'); } catch (_) { /* 忽略 */ }
    }
    device = ws;
    deviceBlocked = false;
    console.log('[device] 已連線');
    setDeviceOnline(true);
    safeSend(ws, { t: 'hello', fw: 'server' });

    ws.on('message', (raw) => {
      let m;
      try { m = JSON.parse(raw.toString()); } catch (_) { return; }
      if (m.t === 'status') {
        lastStatus = {
          online: true,
          free: Number(m.free) || 0,
          total: Number(m.total) || 8,
          busy: !!m.busy,
          cur: Number(m.cur) || 1,
          codes: Array.isArray(m.codes) ? m.codes.map(String) : []
        };
        broadcast({ t: 'status', ...lastStatus });
      } else if (m.t === 'result') {
        if (!commandInFlight) {
          console.warn('[device] 收到沒有對應命令的結果');
          return;
        }
        const command = commandInFlight;
        clearTimeout(command.timer);
        commandInFlight = null;
        sendResult(command.ws, command.id, { ok: !!m.ok, msg: m.msg || '', slot: m.slot, code: m.code });
        pumpDeviceQueue();
      }
    });

    ws.on('close', () => {
      if (device === ws) {
        device = null;
        console.log('[device] 離線');
        discardDeviceCommands('櫃子連線中斷，請稍後再試');
        setDeviceOnline(false);
      }
    });
    return;
  }

  /* --- 用戶端（手機/電腦瀏覽器） --- */
  users.add(ws);
  console.log('[user] 連入，共 ' + users.size + ' 人');
  safeSend(ws, { t: 'status', ...lastStatus, online: !!device });

  ws.on('message', (raw) => {
    let m;
    try { m = JSON.parse(raw.toString()); } catch (_) { return; }
    const id = m.id;
    if (!Number.isInteger(id)) return;

    if (m.t === 'store' || m.t === 'take') {
      if (m.t === 'take') {
        const code = String(m.code || '').trim();
        if (!/^\d{4}$/.test(code)) { sendResult(ws, id, { ok: false, msg: '請輸入 4 位取傘碼' }); return; }
        enqueueDeviceCommand(ws, id, 'take ' + code);
      } else {
        enqueueDeviceCommand(ws, id, 'store');
      }
      return;
    }

    if (m.t === 'admin') {
      if (String(m.key || '') !== ADMIN_KEY) {
        safeSend(ws, { t: 'result', id, ok: false, msg: '管理密碼錯誤' });
        return;
      }
      const act = String(m.act || '');
      const n = Number(m.n);
      const command = ['open', 'close', 'clear'].includes(act)
        ? act
        : act === 'home' || act === 'go'
          ? Number.isInteger(n) && n >= 1 && n <= 8 ? act + ' ' + n : null
        : null;
      if (command) {
        enqueueDeviceCommand(ws, id, command);
      } else if (act === 'home' || act === 'go') {
        sendResult(ws, id, { ok: false, msg: '格號必須是 1-8' });
      } else {
        sendResult(ws, id, { ok: false, msg: '不支援的操作' });
      }
    }
  });

  ws.on('close', () => {
    users.delete(ws);
    cancelQueuedCommandsFor(ws);
    console.log('[user] 離線，剩餘 ' + users.size + ' 人');
  });
});

/* ---------- 心跳：保持連線活跃（穿過代理/NAT） ---------- */
const heartbeat = setInterval(() => {
  wss.clients.forEach((ws) => {
    if (ws.isAlive === false) { try { ws.terminate(); } catch (_) { /* 忽略 */ } return; }
    ws.isAlive = false;
    try { ws.ping(); } catch (_) { /* 忽略 */ }
  });
}, 30000);

server.listen(PORT, '0.0.0.0', () => {
  console.log('==============================================');
  console.log(' 智能雨傘寄存櫃 — 在線控制伺服器已啟動');
  console.log(' 本機測試：http://localhost:' + PORT);
  console.log(' 設備密鑰：' + (process.env.DEVICE_SECRET ? '(已設定)' : '(預設值，請務必修改)'));
  console.log('==============================================');
});

function shutdown(signal) {
  console.log('[server] 收到 ' + signal + '，正在優雅關閉');
  clearInterval(heartbeat);
  discardDeviceCommands('伺服器正在重啟，請稍後再試');
  for (const ws of wss.clients) {
    try { ws.close(1001, 'server shutdown'); } catch (_) { /* 忽略 */ }
  }
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 10000).unref();
}

process.once('SIGTERM', () => shutdown('SIGTERM'));
process.once('SIGINT', () => shutdown('SIGINT'));
