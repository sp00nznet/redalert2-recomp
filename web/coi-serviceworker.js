/*
 * coi-serviceworker.js - the two headers the game needs (it runs on
 * threads, which need SharedArrayBuffer, which needs cross-origin
 * isolation), added by a service worker for a host that cannot send them
 * itself (GitHub Pages). A host that sends them (tools/serve_web.py, a
 * _headers file on Netlify or Cloudflare Pages) never registers this.
 */
if (typeof window === 'undefined') {
    self.addEventListener('install', function () { self.skipWaiting(); });
    self.addEventListener('activate', function (e) { e.waitUntil(self.clients.claim()); });
    self.addEventListener('fetch', function (e) {
        var req = e.request;
        if (req.cache === 'only-if-cached' && req.mode !== 'same-origin') return;
        e.respondWith(fetch(req).then(function (res) {
            if (res.status === 0 || res.type === 'opaque') return res;
            var h = new Headers(res.headers);
            h.set('Cross-Origin-Embedder-Policy', 'require-corp');
            h.set('Cross-Origin-Opener-Policy', 'same-origin');
            h.set('Cross-Origin-Resource-Policy', 'same-origin');
            return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
        }));
    });
} else if (!window.crossOriginIsolated && 'serviceWorker' in navigator && window.isSecureContext) {
    navigator.serviceWorker.register(document.currentScript.src).then(function (reg) {
        /* the first load is not isolated: once the worker is in charge, load again */
        if (!navigator.serviceWorker.controller) {
            navigator.serviceWorker.addEventListener('controllerchange', function () { window.location.reload(); });
            if (reg.active && !navigator.serviceWorker.controller) window.location.reload();
        }
    }).catch(function (e) { console.warn('service worker:', e); });
}
