/* 瀏覽器模擬測試：用 jsdom 載入線上 index.html + app.js，
   注入真實 WebSocket（連線上伺服器），複刻手機瀏覽器行為。
   目的：抓 app.js 在瀏覽器環境的執行期錯誤（例如 $('id') 為 null）。 */
'use strict';
const fs = require('fs');
const { JSDOM } = require('jsdom');
const WS = require('ws');

const ORIGIN = 'https://umbrellahub-caozd3vf.manus.space';
const html = fs.readFileSync('C:/Users/user/WorkBuddy/2026-10-20-11-12-36/umbrella-online/public/index.html', 'utf8');
const appSrc = fs.readFileSync('C:/Users/user/WorkBuddy/2026-10-20-11-12-36/umbrella-online/public/app.js', 'utf8');

/* 不載入外部資源（Manus 注入的分析腳本），只建 DOM */
const dom = new JSDOM(html, {
  url: ORIGIN + '/',
  runScripts: 'outside-only',
  pretendToBeVisual: true
});
const win = dom.window;

/* 注入瀏覽器相容的 WebSocket（打真實伺服器） */
const events = [];
class BrowserWS {
  constructor(url) {
    events.push('new WebSocket(' + url + ')');
    this.url = url;
    this.readyState = 0;
    this._ws = new WS(url, { origin: ORIGIN });
    this._ws.on('open', () => { this.readyState = 1; events.push('ws open'); if (this.onopen) this.onopen(); });
    this._ws.on('message', (raw) => { events.push('ws message: ' + raw.toString().slice(0, 60)); if (this.onmessage) this.onmessage({ data: raw.toString() }); });
    this._ws.on('close', (code) => { this.readyState = 3; events.push('ws close ' + code); if (this.onclose) this.onclose({ code }); });
    this._ws.on('error', (e) => { events.push('ws error: ' + e.message); if (this.onerror) this.onerror(e); });
  }
  send(d) { events.push('ws send: ' + d.slice(0, 40)); this._ws.send(d); }
  close() { this._ws.close(); }
}
win.WebSocket = BrowserWS;

/* 補齊瀏覽器 API */
Object.defineProperty(win.navigator, 'serviceWorker', { value: undefined, configurable: true });
try { Object.defineProperty(win.navigator, 'vibrate', { value: () => true, configurable: true }); } catch (e) { /* 忽略 */ }

/* 載入 app.js 並觸發 DOMContentLoaded */
try {
  win.eval(appSrc);
  events.push('app.js 載入成功');
} catch (e) {
  events.push('❌ app.js 載入即報錯: ' + e.message);
}
try {
  win.document.dispatchEvent(new win.Event('DOMContentLoaded', { bubbles: true }));
  events.push('DOMContentLoaded 已觸發');
} catch (e) {
  events.push('❌ DOMContentLoaded 處理器報錯: ' + e.message);
}

/* 觀察 10 秒 */
setTimeout(() => {
  const conn = win.document.getElementById('conn');
  const free = win.document.getElementById('free');
  events.push('--- 10 秒後狀態 ---');
  events.push('conn  pill: "' + conn.textContent + '" class="' + conn.className + '"');
  events.push('free 數字: "' + free.textContent + '"');
  console.log(events.join('\n'));
  const connected = events.some(e => e.startsWith('ws open'));
  const gotStatus = events.some(e => e.startsWith('ws message'));
  console.log('\n===== 判定 =====');
  console.log('WS 是否連上:', connected ? '✅' : '❌');
  console.log('是否收到狀態:', gotStatus ? '✅' : '❌');
  const err = events.find(e => e.includes('報錯'));
  console.log('執行錯誤:', err || '無');
  process.exit(connected && gotStatus && !err ? 0 : 1);
}, 10000);
