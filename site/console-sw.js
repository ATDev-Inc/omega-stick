"use strict";
// Service worker for the hosted console, published as /omega-stick/console/sw.js
// so it covers /omega-stick/console/ only. It keeps the console page working
// with no internet after the first visit. Network first: while online the page
// always comes fresh from the server (it must match the firmware protocol), and
// the cached copy is used only when the network fails. The firmware feed and
// images are never intercepted or cached.

// Cache names are shared by every site on the origin (other project pages on
// the same github.io host), so this worker only ever touches names that start
// with its own prefix.
const CACHE_PREFIX = "omega-console-";
const CACHE = CACHE_PREFIX + "v1";

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

// Whether a cache is an older one of this worker's, safe to delete on
// activation. Any name without the prefix belongs to someone else.
function isObsoleteConsoleCache(name) {
  return typeof name === "string" && name.startsWith(CACHE_PREFIX) && name !== CACHE;
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
        .then((names) => Promise.all(names.filter(isObsoleteConsoleCache).map((n) => caches.delete(n))))
        .then(() => self.clients.claim()));
  });

  self.addEventListener("fetch", (event) => {
    const req = event.request;
    if (!isConsoleDocument(req.method, req.url, self.registration.scope)) return;
    event.respondWith((async () => {
      let resp;
      try {
        resp = await fetch(req, { cache: "no-store" });
      } catch (e) {
        // Only a failed network request falls back to the saved copy.
        let cached;
        try {
          const cache = await caches.open(CACHE);
          cached = await cache.match(new Request(self.registration.scope));
        } catch (_) { /* no usable cache: report the network failure */ }
        if (cached) return cached;
        throw e;
      }
      if (resp.ok) {
        // Saving the copy is best effort. A failed write (storage full, for
        // example) must never replace the fresh page with an older one.
        try {
          const cache = await caches.open(CACHE);
          await cache.put(new Request(self.registration.scope), resp.clone());
        } catch (_) { /* keep the previous copy */ }
      }
      return resp;
    })());
  });
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = { CACHE, CACHE_PREFIX, isConsoleDocument, isObsoleteConsoleCache };
}
