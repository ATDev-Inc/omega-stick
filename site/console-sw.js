"use strict";
// Service worker for the hosted console, published as /omega-stick/console/sw.js
// so it covers /omega-stick/console/ only. It keeps the console page working
// with no internet after the first visit. Network first: while online the page
// always comes fresh from the server (it must match the firmware protocol), and
// the cached copy is used only when the network fails. The firmware feed and
// images are never intercepted or cached.

const CACHE = "omega-console-v1";

// Whether a request is the console document itself: a same-origin GET for the
// scope root or its index.html. The query string and hash are ignored.
// scope: the registration scope URL.
function isConsoleDocument(method, url, scope) {
  if (method !== "GET") return false;
  try {
    const u = new URL(String(url));
    const s = new URL(String(scope));
    if (u.origin !== s.origin) return false;
    return u.pathname === s.pathname || u.pathname === s.pathname + "index.html";
  } catch (_) { return false; }
}

if (typeof self !== "undefined" && typeof self.addEventListener === "function" && self.registration) {
  self.addEventListener("install", (event) => {
    event.waitUntil(
      caches.open(CACHE)
        .then((cache) => cache.add(new Request("./", { cache: "reload" })))
        .then(() => self.skipWaiting()));
  });

  self.addEventListener("activate", (event) => {
    event.waitUntil(
      caches.keys()
        .then((names) => Promise.all(names.filter((n) => n !== CACHE).map((n) => caches.delete(n))))
        .then(() => self.clients.claim()));
  });

  self.addEventListener("fetch", (event) => {
    const req = event.request;
    if (!isConsoleDocument(req.method, req.url, self.registration.scope)) return;
    event.respondWith((async () => {
      const cache = await caches.open(CACHE);
      try {
        const resp = await fetch(req, { cache: "no-store" });
        if (resp.ok) await cache.put(new Request(self.registration.scope), resp.clone());
        return resp;
      } catch (e) {
        const cached = await cache.match(new Request(self.registration.scope));
        if (cached) return cached;
        throw e;
      }
    })());
  });
}

if (typeof module !== "undefined" && module.exports) module.exports = { CACHE, isConsoleDocument };
