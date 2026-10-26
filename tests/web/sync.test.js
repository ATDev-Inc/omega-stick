"use strict";
// calibration_console.html is distributed as a single standalone file (opened
// from disk), so omega_protocol.js is inlined into it. These tests keep the
// inline copy identical to the source file and check the page only uses names
// the module actually exports.

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const webDir = path.join(__dirname, "..", "..", "components", "omega_calibration", "web");
const js = fs.readFileSync(path.join(webDir, "omega_protocol.js"), "utf8");
const html = fs.readFileSync(path.join(webDir, "calibration_console.html"), "utf8");
const P = require(path.join(webDir, "omega_protocol.js"));

const BEGIN = "/* BEGIN omega_protocol.js */\n";
const END = "/* END omega_protocol.js */";

test("inline protocol block in the HTML is identical to omega_protocol.js", () => {
  const b = html.indexOf(BEGIN);
  const e = html.indexOf(END);
  assert.ok(b >= 0 && e > b, "BEGIN/END marker comments missing from calibration_console.html");
  const inline = html.slice(b + BEGIN.length, e);
  assert.equal(
    inline, js,
    "The inline block in calibration_console.html differs from omega_protocol.js. " +
    "Copy the contents of omega_protocol.js verbatim between the BEGIN/END markers in the HTML.");
});

test("omega_protocol.js has no DOM access and no closing script tag", () => {
  assert.ok(!js.includes("</script"), "would terminate the inline <script> block");
  for (const word of ["document.", "window.", "els.", "logLine", "navigator."]) {
    assert.ok(!js.includes(word), "DOM/global coupling found: " + word);
  }
});

test("every OmegaProtocol name the page destructures is exported", () => {
  const m = html.match(/const \{([^}]*)\} = globalThis\.OmegaProtocol;/);
  assert.ok(m, "page does not destructure globalThis.OmegaProtocol");
  const names = m[1].split(",").map((s) => s.trim()).filter(Boolean);
  assert.ok(names.length > 0);
  for (const n of names) assert.ok(n in P, "page uses missing export: " + n);
});

test("the page no longer defines its own copies of the protocol functions", () => {
  for (const fn of ["crc32", "buildFrame", "parseErrorReply", "parseDiscovery", "decodeState"]) {
    const defs = html.match(new RegExp("function " + fn + "\\(", "g")) || [];
    assert.equal(defs.length, 1, fn + " should be defined once (the inline module copy)");
  }
});

// setUsbUpdates(false) hides #fwControls. The result of an update that just
// finished (#updateStatus) must not sit under it, nor the running version.
test("the hidden update controls do not contain the update result or the version", () => {
  const tagRe = /<(\/?)(\w+)([^>]*)>/g;
  const voids = new Set(["input", "br", "hr", "img", "meta", "link"]);
  const stack = [];
  const ancestors = {};
  let m;
  const body = html.slice(html.indexOf("<body"), html.indexOf("<script"));
  while ((m = tagRe.exec(body))) {
    const [, close, name, attrs] = m;
    if (close) {
      while (stack.length && stack.pop().name !== name) { /* unwind */ }
      continue;
    }
    const id = (/\bid="([^"]+)"/.exec(attrs) || [])[1];
    if (id) ancestors[id] = stack.map((s) => s.id).filter(Boolean);
    if (!voids.has(name) && !attrs.endsWith("/")) stack.push({ name, id });
  }
  assert.ok(ancestors.fwControls, "#fwControls not found");
  for (const id of ["updateStatus", "updateMessage", "updateError", "fwProgress", "fwReconnectBtn",
                    "fwRunning", "fwVersion", "fwNoUpdates"]) {
    assert.ok(ancestors[id], "#" + id + " not found");
    assert.ok(!ancestors[id].includes("fwControls"), "#" + id + " is inside #fwControls");
  }
  for (const id of ["fwDrop", "fwFile", "flashBtn", "latestCheckBtn", "latestInstallBtn", "fwRevertBtn"]) {
    assert.ok(ancestors[id].includes("fwControls"), "#" + id + " should be inside #fwControls");
  }
});

test("no function name is declared twice at the same indent in one script block", () => {
  // A second `function hex` once silently replaced the first, so the frame
  // log dumped whole OTA chunks. Same-indent duplicates share a scope here.
  const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)];
  assert.ok(scripts.length > 0, "no <script> block found");
  const dups = [];
  scripts.forEach((m, i) => {
    const seen = new Set();
    for (const d of m[1].matchAll(/^( *)(?:async +)?function\s+([A-Za-z_$][\w$]*)/gm)) {
      const key = d[1].length + ":" + d[2];
      if (seen.has(key)) dups.push("script " + i + ": " + d[2]);
      seen.add(key);
    }
  });
  assert.deepEqual(dups, []);
});
