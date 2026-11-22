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
