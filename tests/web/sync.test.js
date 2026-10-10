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
