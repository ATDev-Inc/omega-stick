"use strict";
// Offline console: when the page registers its service worker, and which
// requests the worker (site/console-sw.js) handles.

const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");

const P = require(path.join(__dirname, "..", "..", "components", "omega_calibration", "web", "omega_protocol.js"));
const SW = require(path.join(__dirname, "..", "..", "site", "console-sw.js"));

test("shouldRegisterConsoleSw: only the hosted console over https", () => {
  const ok = (u) => P.shouldRegisterConsoleSw(u, true);
  assert.equal(ok("https://atdev-inc.github.io/omega-stick/console/"), true);
  assert.equal(ok("https://atdev-inc.github.io/omega-stick/console/index.html?x=1#y"), true);
  assert.equal(ok("https://atdev-inc.github.io/omega-stick/console/?x=1"), true);
  assert.equal(ok("file:///C:/x/calibration_console.html"), false);
  assert.equal(ok("http://127.0.0.1:8000/console/"), false);
  assert.equal(ok("http://localhost:8000/omega-stick/console/"), false);
  assert.equal(ok("http://atdev-inc.github.io/omega-stick/console/"), false);
  assert.equal(ok("https://example.github.io/omega-stick/console/"), false);
  assert.equal(ok("https://atdev-inc.github.io/omega-stick/"), false);
  assert.equal(ok("https://atdev-inc.github.io/omega-stick/flash.html"), false);
  assert.equal(ok("https://atdev-inc.github.io/other/console/"), false);
  assert.equal(ok("not a url"), false);
  assert.equal(P.shouldRegisterConsoleSw("https://atdev-inc.github.io/omega-stick/console/", false), false);
});

const SCOPE = "https://atdev-inc.github.io/omega-stick/console/";
const handled = (m, u) => SW.isConsoleDocument(m, u, SCOPE);

test("isConsoleDocument: the console page only", () => {
  assert.equal(handled("GET", SCOPE), true);
  assert.equal(handled("GET", SCOPE + "index.html"), true);
  assert.equal(handled("GET", SCOPE + "?x=1#y"), true);
  assert.equal(handled("GET", "https://atdev-inc.github.io/omega-stick/firmware/latest.json"), false);
  assert.equal(handled("GET", "https://atdev-inc.github.io/omega-stick/firmware/omega_stick-0.2.0.bin"), false);
  assert.equal(handled("GET", "https://atdev-inc.github.io/omega-stick/console/sw.js"), false);
  assert.equal(handled("GET", "https://example.com/omega-stick/console/"), false);
  assert.equal(handled("POST", SCOPE), false);
  assert.equal(handled("GET", "garbage"), false);
});

// ---- Worker lifecycle, run against a fake service worker environment ----
// The predicates above do not cover what the handlers do with the cache, so
// these load site/console-sw.js in a sandbox with a fake `self`, `caches` and
// `fetch` and drive its install, activate and fetch listeners directly.

const fs = require("node:fs");
const vm = require("node:vm");
const SW_SRC = fs.readFileSync(path.join(__dirname, "..", "..", "site", "console-sw.js"), "utf8");

class FakeRequest {
  constructor(input, init) {
    this.url = typeof input === "string" ? new URL(input, SCOPE + "sw.js").href : input.url;
    this.method = (init && init.method) || (typeof input === "object" && input.method) || "GET";
  }
}
const page = (body, ok = true) => ({ ok, body, clone() { return page(body, ok); } });

// stores: Map of cache name to Map of url to response, shared by the origin.
function loadWorker({ stores, fetchImpl, failPut = false }) {
  const listeners = {};
  const caches = {
    async open(name) {
      if (!stores.has(name)) stores.set(name, new Map());
      const entries = stores.get(name);
      return {
        async add(req) { entries.set(req.url, page("installed")); },
        async put(req, resp) {
          if (failPut) { const e = new Error("quota"); e.name = "QuotaExceededError"; throw e; }
          entries.set(req.url, resp);
        },
        async match(req) { return entries.get(req.url); },
      };
    },
    async keys() { return [...stores.keys()]; },
    async delete(name) { return stores.delete(name); },
  };
  const self = {
    registration: { scope: SCOPE },
    addEventListener(type, fn) { listeners[type] = fn; },
    async skipWaiting() {},
    clients: { async claim() {} },
  };
  vm.runInNewContext(SW_SRC, { self, caches, fetch: fetchImpl, Request: FakeRequest, URL, module: { exports: {} } });
  const lifecycle = async (type) => {
    let done;
    listeners[type]({ waitUntil(p) { done = p; } });
    await done;
  };
  // Resolves to the response the worker answered with, or undefined when it
  // left the request alone.
  const request = (url, method = "GET") => {
    let answer;
    listeners.fetch({ request: new FakeRequest(url, { method }), respondWith(p) { answer = p; } });
    return answer;
  };
  return { lifecycle, request };
}

test("isObsoleteConsoleCache: only this worker's older caches", () => {
  assert.equal(SW.isObsoleteConsoleCache("omega-console-v0"), true);
  assert.equal(SW.isObsoleteConsoleCache(SW.CACHE), false);
  assert.equal(SW.isObsoleteConsoleCache("other-project-v1"), false);
  assert.equal(SW.isObsoleteConsoleCache(""), false);
  assert.equal(SW.isObsoleteConsoleCache(undefined), false);
});

test("activate: leaves other sites' caches on the origin alone", async () => {
  const stores = new Map([
    ["other-project-v1", new Map([["https://atdev-inc.github.io/other-project/offline.html", page("theirs")]])],
    ["omega-console-v0", new Map([[SCOPE, page("old")]])],
  ]);
  const w = loadWorker({ stores, fetchImpl: async () => page("C") });
  await w.lifecycle("install");
  await w.lifecycle("activate");
  assert.deepEqual([...stores.keys()].sort(), ["omega-console-v1", "other-project-v1"]);
  assert.equal(stores.get("other-project-v1").get("https://atdev-inc.github.io/other-project/offline.html").body, "theirs");
});

test("fetch: a fresh page is returned even when saving it fails", async () => {
  const stores = new Map([[SW.CACHE, new Map([[SCOPE, page("B")]])]]);
  const w = loadWorker({ stores, fetchImpl: async () => page("C"), failPut: true });
  assert.equal((await w.request(SCOPE)).body, "C");
  assert.equal(stores.get(SW.CACHE).get(SCOPE).body, "B"); // the older copy is kept for offline use
});

test("fetch: online refreshes the saved copy, offline uses it", async () => {
  const stores = new Map([[SW.CACHE, new Map([[SCOPE, page("B")]])]]);
  let online = true;
  const w = loadWorker({ stores, fetchImpl: async () => { if (!online) throw new TypeError("Failed to fetch"); return page("C"); } });
  assert.equal((await w.request(SCOPE + "?module=5")).body, "C");
  online = false;
  assert.equal((await w.request(SCOPE + "index.html")).body, "C");
});

test("fetch: an error page is passed through and never saved", async () => {
  const stores = new Map([[SW.CACHE, new Map([[SCOPE, page("B")]])]]);
  const w = loadWorker({ stores, fetchImpl: async () => page("404", false) });
  assert.equal((await w.request(SCOPE)).body, "404");
  assert.equal(stores.get(SW.CACHE).get(SCOPE).body, "B");
});

test("fetch: offline with nothing saved reports the network failure", async () => {
  const w = loadWorker({ stores: new Map(), fetchImpl: async () => { throw new TypeError("Failed to fetch"); } });
  await assert.rejects(w.request(SCOPE), /Failed to fetch/);
});

test("fetch: firmware files and other requests are left alone", () => {
  const w = loadWorker({ stores: new Map(), fetchImpl: async () => page("C") });
  assert.equal(w.request("https://atdev-inc.github.io/omega-stick/firmware/latest.json"), undefined);
  assert.equal(w.request("https://atdev-inc.github.io/omega-stick/firmware/omega_stick-0.2.0.bin"), undefined);
  assert.equal(w.request(SCOPE, "POST"), undefined);
});