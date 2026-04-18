// Minimal service worker: precache shell + stale-while-revalidate for assets.
const CACHE = 'kik-v1';
const ASSETS = [
  '/', '/index.html', '/tv.html', '/instructor.html',
  '/css/main.css', '/js/i18n.js', '/js/ws.js', '/js/app.js',
  '/manifest.webmanifest', '/assets/icon.svg',
];
self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(ASSETS)).then(()=>self.skipWaiting()));
});
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k=>k!==CACHE).map(k=>caches.delete(k)))).then(()=>self.clients.claim()));
});
self.addEventListener('fetch', e => {
  const req = e.request;
  if(req.method !== 'GET') return;
  if(new URL(req.url).pathname.startsWith('/api/')) return; // always network for API
  e.respondWith(
    caches.match(req).then(cached => {
      const net = fetch(req).then(res => {
        if(res && res.ok) caches.open(CACHE).then(c => c.put(req, res.clone()));
        return res;
      }).catch(()=>cached);
      return cached || net;
    })
  );
});
