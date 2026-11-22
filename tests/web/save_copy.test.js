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
  assert.match(P.describeSaveCopy("saved"), new RegExp(P.SAVE_COPY_NAME));
  for (const kind of ["notconsole", "http", "offline", "unreachable"]) {
    const t = P.describeSaveCopy(kind, 404);
    assert.match(t, /nothing was saved/);
    assert.match(t, /releases page on GitHub/);
    assert.ok(!t.includes("\u2014"));
  }
  assert.match(P.describeSaveCopy("http", 404), /404/);
});
