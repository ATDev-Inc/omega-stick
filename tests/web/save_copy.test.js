"use strict";
// "Save a copy of this page": when the button is offered, which address is
// fetched, and the check that the text really is the console.

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const WEB = path.join(__dirname, "..", "..", "components", "omega_calibration", "web");
const P = require(path.join(WEB, "omega_protocol.js"));

test("canSaveCopy: offered for http(s) pages only", () => {
  assert.equal(P.canSaveCopy("https://atdev-inc.github.io/omega-stick/console/"), true);
  assert.equal(P.canSaveCopy("http://localhost:8000/console/"), true);
  assert.equal(P.canSaveCopy("file:///C:/x/omega_stick_console.html"), false);
  assert.equal(P.canSaveCopy("garbage"), false);
  assert.equal(P.canSaveCopy(""), false);
  assert.equal(P.canSaveCopy(undefined), false);
});

test("saveCopyUrl strips the query string and hash, and refuses file://", () => {
  assert.equal(P.saveCopyUrl("https://atdev-inc.github.io/omega-stick/console/?feed=x#y"),
    "https://atdev-inc.github.io/omega-stick/console/");
  assert.equal(P.saveCopyUrl("http://localhost:8000/calibration_console.html?a=1"),
    "http://localhost:8000/calibration_console.html");
  assert.equal(P.saveCopyUrl("file:///C:/x/console.html"), null);
  assert.equal(P.saveCopyUrl("garbage"), null);
});

test("looksLikeConsole: the real page passes, other pages fail", () => {
  const real = fs.readFileSync(path.join(WEB, "calibration_console.html"), "utf8");
  assert.equal(P.looksLikeConsole(real), true);
  assert.equal(P.looksLikeConsole(""), false);
  assert.equal(P.looksLikeConsole(null), false);
  assert.equal(P.looksLikeConsole("<html><title>404 Not Found</title><body>Not here</body></html>"), false);
  assert.equal(P.looksLikeConsole("<html><title>Sign in to Wi-Fi</title><body><form>Accept terms</form></body></html>"), false);
});

test("describeSaveCopy: plain wording, points to the releases page on failure", () => {
  for (const kind of ["notconsole", "http", "offline", "timeout", "unreachable"]) {
    const t = P.describeSaveCopy(kind, 404);
    assert.match(t, /nothing was saved/);
    assert.match(t, /releases page on GitHub/);
    assert.ok(!t.includes("\u2014"));
  }
  assert.match(P.describeSaveCopy("http", 404), /404/);
});

test("describeSaveCopy: a started download is not reported as a saved file", () => {
  // The page hands the file to the browser and cannot see what happens next: a
  // blocked or cancelled download reports nothing back. So the wording must ask
  // the person to check, and must say what to do when the file is not there.
  const t = P.describeSaveCopy("started");
  assert.match(t, new RegExp(P.SAVE_COPY_NAME));
  assert.match(t, /has started/);
  assert.match(t, /Check that the file is in your Downloads folder/);
  assert.match(t, /may have blocked it/);
  assert.match(t, /releases page on GitHub/);
  assert.ok(!/\bSaved\b/.test(t), "must not claim the file was saved");
  assert.ok(!t.includes("\u2014"));
});

// A response whose body never finishes: arrayBuffer() only settles when the
// request is aborted, like a connection that stalls after the headers.
function stalledBody(signal) {
  return new Promise((_, reject) => {
    signal.addEventListener("abort", () => reject(Object.assign(new Error("aborted"), { name: "AbortError" })));
  });
}
const realDeps = (fetchImpl) => ({ fetch: fetchImpl, AbortController, setTimeout, clearTimeout });

test("fetchBytesWithDeadline: a body that stalls is aborted and reported as a timeout", async () => {
  let calls = 0;
  const fetchImpl = async (url, opts) => { calls++; return { ok: true, status: 200, arrayBuffer: () => stalledBody(opts.signal) }; };
  let error;
  try { await P.fetchBytesWithDeadline("https://example.test/console/", 20, realDeps(fetchImpl)); }
  catch (e) { error = e; }
  assert.equal(calls, 1);
  assert.ok(error, "the stalled read must reject, not hang");
  assert.equal(P.classifyFetchError(error, true), "timeout");
  assert.match(P.describeSaveCopy("timeout"), /took too long/);
});

test("fetchBytesWithDeadline: a request that never answers is aborted too", async () => {
  const fetchImpl = (url, opts) => stalledBody(opts.signal);
  await assert.rejects(P.fetchBytesWithDeadline("https://example.test/console/", 20, realDeps(fetchImpl)), { name: "AbortError" });
});

test("fetchBytesWithDeadline: returns the bytes unchanged and clears its timer", async () => {
  const bytes = new TextEncoder().encode("\uFEFF<title>espp Omega Calibration Console</title>").buffer;
  let cleared = 0, armed = 0, seen;
  const deps = {
    fetch: async (url, opts) => { seen = { url, opts }; return { ok: true, status: 200, arrayBuffer: async () => bytes }; },
    AbortController,
    setTimeout: (f, ms) => { armed++; return setTimeout(f, ms); },
    clearTimeout: (t) => { cleared++; clearTimeout(t); },
  };
  const res = await P.fetchBytesWithDeadline("https://example.test/console/", 1000, deps);
  assert.equal(res.ok, true);
  assert.equal(res.bytes, bytes); // the same buffer, byte order mark included
  assert.equal(seen.opts.cache, "no-store");
  assert.ok(seen.opts.signal);
  assert.deepEqual([armed, cleared], [1, 1]);
});

test("fetchBytesWithDeadline: an error status has no bytes", async () => {
  const res = await P.fetchBytesWithDeadline("https://example.test/console/", 1000,
    realDeps(async () => ({ ok: false, status: 503, arrayBuffer: async () => { throw new Error("must not be read"); } })));
  assert.deepEqual(res, { ok: false, status: 503 });
});