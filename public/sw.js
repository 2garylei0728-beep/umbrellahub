/* 智能旋轉式雨僘寄存櫃 — Service Worker（PWA 離線殼）
   策略：
   - 頁面導航（HTML）：網絡優先，失敗取離線殼（斷網也能開頁）
   - 靜態資源（app.js / icon / manifest）：緩存優先 + 背景更新（重複打開秒開）
   - /api/ 與 /ws：完全不經 SW，確保狀態永遠最新 */
'use strict';

const VERSION = 'umbrella-v2';
const SHELL = ['./', './index.html', './app.js', './manifest.webmanifest', './icon.svg'];

self.addEventListener('install', (e) => {
  e.waitUntil(
    caches.open(VERSION).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== VERSION).map((k) => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;
  if (url.pathname.startsWith('/api/') || url.pathname.startsWith('/ws')) return;

  if (req.mode === 'navigate') {
    /* 網絡優先：保證拿到最新版頁面；離線時用殼 */
    e.respondWith(
      fetch(req).then((res) => {
        const copy = res.clone();
        caches.open(VERSION).then((c) => c.put('./index.html', copy));
        return res;
      }).catch(() => caches.match('./index.html').then((r) => r || caches.match('./')))
    );
    return;
  }

  /* JS（app.js）：網絡優先——永遠拿最新代碼，消滅緩存舊版風險 */
  if (url.pathname.endsWith('.js')) {
    e.respondWith(
      fetch(req).then((res) => {
        const copy = res.clone();
        caches.open(VERSION).then((c) => c.put(req, copy));
        return res;
      }).catch(() => caches.match(req))
    );
    return;
  }

  /* 其餘靜態資源（icon/manifest）：緩存優先 + 背景更新 */
  e.respondWith(
    caches.match(req).then((hit) => {
      const net = fetch(req).then((res) => {
        if (res && res.ok) {
          const copy = res.clone();
          caches.open(VERSION).then((c) => c.put(req, copy));
        }
        return res;
      }).catch(() => hit);
      return hit || net;
    })
  );
});
