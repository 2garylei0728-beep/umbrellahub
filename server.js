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
  res.json({ ok: true, deviceOnline: !!device, users: users.size, uptime: Math.round(process.uptime()) });
});

app.get('/api/status', (req, res) => {
  res.set('Cache-Control', 'no-store');
  res.json(Object.assign({}, lastStatus, { online: !!device }));
});

const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/ws' });

/* ---------- 狀態 ---------- */
let device = null;                 // 設備（ESP32）連線，單設備設計
const users = new Set();           // 所有手機/電腦瀏覽器
let lastStatus = { online: false, free: 8, total: 8, busy: false, cur: 1, codes: [] };
let pendingUserId = null;           // ESP32 韌體不回傳 request id，單設備／單動作時由伺服器配對

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
      try { device.close(4002, 'replaced'); } catch (_) { /* 忽略 */ }
    }
    device = ws;
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
        const result = { t: 'result', id: pendingUserId, ok: !!m.ok, msg: m.msg || '', slot: m.slot, code: m.code };
        pendingUserId = null;
        broadcast(result);
      }
    });

    ws.on('close', () => {
      if (device === ws) {
        device = null;
        console.log('[device] 離線');
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

    if (m.t === 'store' || m.t === 'take') {
      if (!device) { safeSend(ws, { t: 'result', id, ok: false, msg: '櫃子暫時離線，請稍後再試' }); return; }
      pendingUserId = id;
      if (m.t === 'take') {
        const code = String(m.code || '').trim();
        if (!/^\d{4}$/.test(code)) { safeSend(ws, { t: 'result', id, ok: false, msg: '請輸入 4 位取傘碼' }); return; }
        if (!sendToDevice('take ' + code)) { pendingUserId = null; safeSend(ws, { t: 'result', id, ok: false, msg: '傳送失敗，請重試' }); return; }
      } else {
        if (!sendToDevice('store')) { pendingUserId = null; safeSend(ws, { t: 'result', id, ok: false, msg: '傳送失敗，請重試' }); return; }
      }
      return;
    }

    if (m.t === 'admin') {
      if (String(m.key || '') !== ADMIN_KEY) {
        safeSend(ws, { t: 'result', id, ok: false, msg: '管理密碼錯誤' });
        return;
      }
      const act = String(m.act || '');
      const command = ['open', 'close', 'clear'].includes(act)
        ? act
        : act === 'home' ? 'home ' + (Number(m.n) || 0)
        : act === 'go' ? 'go ' + (Number(m.n) || 0)
        : null;
      if (command) {
        pendingUserId = id;
        if (!sendToDevice(command)) {
          pendingUserId = null;
          safeSend(ws, { t: 'result', id, ok: false, msg: '櫃子暫時離線' });
        }
      } else {
        safeSend(ws, { t: 'result', id, ok: false, msg: '不支援的操作' });
      }
    }
  });

  ws.on('close', () => {
    users.delete(ws);
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

process.on('SIGTERM', () => { clearInterval(heartbeat); server.close(() => process.exit(0)); });
