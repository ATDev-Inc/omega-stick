"use strict";
// Simulator support in omega_protocol.js: message encode/parse, script
// validation (rules from docs/simulator.md) and the HID accumulator.

const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const fs = require("node:fs");

const P = require(path.join(__dirname, "..", "..", "components", "omega_calibration", "web", "omega_protocol.js"));

const SPEC_EXAMPLE = {
  version: 1,
  name: "first boot calibrates and then moves right",
  steps: [
    { at_ms: 0, pattern: "calibrate" },
    { at_ms: 40000, expect: { calibrated: true, center_max: 0.05 } },
    { at_ms: 41000, stick: [0, 0] },
    { at_ms: 41500, stick: [0.6, 0] },
    { at_ms: 43000, expect: { mouse_moved: "right" } },
    { at_ms: 44000, action: "recalibrate" },
    { at_ms: 44100, action: "recalibrate", expect_reply: "busy" },
  ],
};

const withSteps = (...steps) => ({ version: 1, steps });
const ok = (script) => {
  const r = P.validateSimScript(script);
  assert.ok(r.ok, r.errors.join(" "));
};
const bad = (script, fragment) => {
  const r = P.validateSimScript(script);
  assert.equal(r.ok, false, "expected invalid: " + JSON.stringify(script));
  assert.ok(r.errors.length > 0);
  if (fragment) assert.ok(r.errors.join("\n").includes(fragment), r.errors.join("\n") + " should mention " + fragment);
};

// ---- encodeSimMessage ----

test("encodeSimMessage builds each control message", () => {
  const enc = (m) => JSON.parse(P.encodeSimMessage(m));
  assert.deepEqual(enc({ type: "hello" }), { type: "hello", client: "console", version: 1 });
  assert.deepEqual(enc({ type: "stick", x: 0.4, y: -0.1 }), { type: "stick", x: 0.4, y: -0.1 });
  assert.deepEqual(enc({ type: "pattern", name: "circle" }), { type: "pattern", name: "circle" });
  assert.deepEqual(enc({ type: "stop" }), { type: "stop" });
  assert.deepEqual(enc({ type: "reset" }), { type: "reset" });
  assert.deepEqual(enc({ type: "wipe" }), { type: "wipe" });
  assert.deepEqual(enc({ type: "fault", sensor: "no_response" }), { type: "fault", sensor: "no_response" });
  assert.deepEqual(enc({ type: "drift", rate: 0.02 }), { type: "drift", rate: 0.02 });
  assert.deepEqual(enc({ type: "noise", sigma: 0.002 }), { type: "noise", sigma: 0.002 });
  assert.deepEqual(enc({ type: "next_image", behavior: "fail_self_test" }), { type: "next_image", behavior: "fail_self_test" });
  assert.deepEqual(enc({ type: "script", script: SPEC_EXAMPLE }), { type: "script", script: SPEC_EXAMPLE });
});

test("encodeSimMessage clamps stick values to -1..1", () => {
  assert.deepEqual(JSON.parse(P.encodeSimMessage({ type: "stick", x: 4, y: -9 })), { type: "stick", x: 1, y: -1 });
});

test("encodeSimMessage rejects NaN, infinity, and malformed input", () => {
  assert.throws(() => P.encodeSimMessage({ type: "stick", x: NaN, y: 0 }), /finite/);
  assert.throws(() => P.encodeSimMessage({ type: "stick", x: 0, y: Infinity }), /finite/);
  assert.throws(() => P.encodeSimMessage({ type: "stick", x: "0.1", y: 0 }), /finite/);
  assert.throws(() => P.encodeSimMessage({ type: "stick", x: 0 }), /finite/);
  assert.throws(() => P.encodeSimMessage({ type: "pattern", name: "spin" }), /pattern name/);
  assert.throws(() => P.encodeSimMessage({ type: "fault", sensor: "melted" }), /sensor fault/);
  assert.throws(() => P.encodeSimMessage({ type: "drift", rate: NaN }), /finite/);
  assert.throws(() => P.encodeSimMessage({ type: "drift", rate: -1 }), /drift/);
  assert.throws(() => P.encodeSimMessage({ type: "noise", sigma: 5 }), /noise/);
  assert.throws(() => P.encodeSimMessage({ type: "next_image", behavior: "explode" }), /next image/);
  assert.throws(() => P.encodeSimMessage({ type: "script", script: { version: 2, steps: [] } }), /not valid/);
  assert.throws(() => P.encodeSimMessage({ type: "bogus" }), /unknown/);
  assert.throws(() => P.encodeSimMessage(null), /type/);
  assert.throws(() => P.encodeSimMessage({}), /type/);
});

// ---- parseSimMessage ----

test("parseSimMessage parses the telemetry messages in the spec", () => {
  assert.deepEqual(
    P.parseSimMessage('{"type":"hello","sim":"omega_sim","version":1,"fw":"0.1.0+sim","mode":"mouse","slot":"ota_0"}'),
    { type: "hello", sim: "omega_sim", version: 1, fw: "0.1.0+sim", mode: "mouse", slot: "ota_0" });
  assert.deepEqual(
    P.parseSimMessage('{"type":"hid","mode":"mouse","dx":3,"dy":-1,"wheel":0,"buttons":1}'),
    { type: "hid", mode: "mouse", dx: 3, dy: -1, wheel: 0, buttons: 1 });
  assert.deepEqual(
    P.parseSimMessage('{"type":"hid","mode":"xac","x":0.25,"y":-0.5}'),
    { type: "hid", mode: "xac", x: 0.25, y: -0.5 });
  assert.deepEqual(
    P.parseSimMessage('{"type":"sensor","x":0.4,"y":-0.1,"bx":12.3,"by":-3.1,"ok":true,"fault":"ok"}'),
    { type: "sensor", x: 0.4, y: -0.1, bx: 12.3, by: -3.1, ok: true, fault: "ok" });
  assert.deepEqual(
    P.parseSimMessage('{"type":"source","kind":"pattern","name":"circle","t_ms":4200,"done":false}'),
    { type: "source", kind: "pattern", name: "circle", t_ms: 4200, done: false });
  assert.deepEqual(P.parseSimMessage('{"type":"rebooting","reason":"mode switch"}'), { type: "rebooting", reason: "mode switch" });
  assert.deepEqual(P.parseSimMessage('{"type":"error","message":"nope"}'), { type: "error", message: "nope" });
});

test("parseSimMessage ignores unknown types and rejects malformed bodies", () => {
  assert.equal(P.parseSimMessage('{"type":"future_thing","a":1}'), null);
  assert.equal(P.parseSimMessage("not json"), null);
  assert.equal(P.parseSimMessage("[1,2]"), null);
  assert.equal(P.parseSimMessage("null"), null);
  assert.equal(P.parseSimMessage('{"x":1}'), null);
  assert.equal(P.parseSimMessage('{"type":"hid","mode":"mouse","dx":"a","dy":0}'), null);
  assert.equal(P.parseSimMessage('{"type":"hid","mode":"joystick"}'), null);
  assert.equal(P.parseSimMessage('{"type":"sensor","x":null,"y":0}'), null);
  assert.equal(P.parseSimMessage('{"type":"source","kind":"telepathy"}'), null);
});

test("parseSimMessage clamps xac values and defaults optional fields", () => {
  assert.deepEqual(P.parseSimMessage('{"type":"hid","mode":"xac","x":3,"y":-3}'), { type: "hid", mode: "xac", x: 1, y: -1 });
  assert.deepEqual(P.parseSimMessage('{"type":"hid","mode":"mouse","dx":1,"dy":2}'),
    { type: "hid", mode: "mouse", dx: 1, dy: 2, wheel: 0, buttons: 0 });
  assert.equal(P.parseSimMessage('{"type":"rebooting"}').reason, "");
});

// ---- validateSimScript ----

test("the spec example script is valid", () => {
  const r = P.validateSimScript(SPEC_EXAMPLE);
  assert.deepEqual(r, { ok: true, errors: [] });
});

test("fw_info and the unsupported reply are valid and encode", () => {
  const step = { at_ms: 1, action: "fw_info", expect_reply: "unsupported" };
  ok(withSteps(step));
  const enc = JSON.parse(P.encodeSimMessage({ type: "script", script: withSteps(step) }));
  assert.equal(enc.script.steps[0].action, "fw_info");
  assert.equal(enc.script.steps[0].expect_reply, "unsupported");
});

test("a script with every kind of step is valid", () => {
  const r = P.validateSimScript(withSteps(
    { at_ms: 0, stick: [-1, 1] },
    { at_ms: 0, pattern: "wander", duration_ms: 500 },
    { at_ms: 5, fault: "weak_magnet" },
    { at_ms: 6, drift: 0 },
    { at_ms: 7, noise: 0.002 },
    { at_ms: 8, next_image: "crash_loop" },
    { at_ms: 9, action: "save", expect_reply: "ok" },
    { at_ms: 10, action: "restart" },
    { at_ms: 11, action: "set_mode", value: "xac" },
    { at_ms: 12, action: "set_deadzone", value: { center: 0.1, range: 0.05 } },
    { at_ms: 13, action: "set_deadzone", value: { range: 0.05 } },
    { at_ms: 14, expect: {
      calibrated: false, center_max: 0.05, mouse_moved: "none", clicked: "left", mode: "mouse",
      sensor_fault: true, neutral: true, fw_slot: "ota_1", fw_pending: false } },
  ));
  assert.deepEqual(r.errors, []);
  assert.equal(r.ok, true);
});

test("validateSimScript rejects a bad envelope", () => {
  bad(null);
  bad([]);
  bad("script");
  bad({ steps: [] }, "version");
  bad({ version: 2, steps: [] }, "version");
  bad({ version: 1 }, "steps");
  bad({ version: 1, steps: {} }, "steps");
  bad({ version: 1, steps: [], extra: 1 }, "extra");
  bad({ version: 1, name: 5, steps: [] }, "name");
});

test("validateSimScript requires at_ms, in time order", () => {
  bad(withSteps({ pattern: "rest" }), "at_ms");
  bad(withSteps({ at_ms: -1, pattern: "rest" }), "at_ms");
  bad(withSteps({ at_ms: "0", pattern: "rest" }), "at_ms");
  bad(withSteps({ at_ms: NaN, pattern: "rest" }), "at_ms");
  bad(withSteps({ at_ms: 100, pattern: "rest" }, { at_ms: 50, pattern: "circle" }), "time order");
  bad({ version: 1, steps: [3] }, "object");
});

test("validateSimScript requires exactly one action key per step", () => {
  bad(withSteps({ at_ms: 0 }), "needs one of");
  bad(withSteps({ at_ms: 0, pattern: "rest", fault: "ok" }), "more than one");
  bad(withSteps({ at_ms: 0, stick: [0, 0], expect: { neutral: true } }), "more than one");
  bad(withSteps({ at_ms: 0, pattern: "rest", bogus: 1 }), "bogus");
  bad(withSteps({ at_ms: 0, stick: [0, 0], duration_ms: 5 }), "duration_ms");
  bad(withSteps({ at_ms: 0, expect: { neutral: true }, value: 1 }), "value");
});

test("validateSimScript checks names, values and ranges", () => {
  bad(withSteps({ at_ms: 0, stick: [0] }), "stick");
  bad(withSteps({ at_ms: 0, stick: [0, 2] }), "stick");
  bad(withSteps({ at_ms: 0, stick: [0, NaN] }), "stick");
  bad(withSteps({ at_ms: 0, stick: "up" }), "stick");
  bad(withSteps({ at_ms: 0, pattern: "spin" }), "pattern");
  bad(withSteps({ at_ms: 0, pattern: "rest", duration_ms: -1 }), "duration_ms");
  bad(withSteps({ at_ms: 0, fault: "melted" }), "fault");
  bad(withSteps({ at_ms: 0, drift: "fast" }), "drift");
  bad(withSteps({ at_ms: 0, drift: -1 }), "drift");
  bad(withSteps({ at_ms: 0, noise: 2 }), "noise");
  bad(withSteps({ at_ms: 0, next_image: "explode" }), "next_image");
});

test("validateSimScript checks action steps", () => {
  bad(withSteps({ at_ms: 0, action: "dance" }), "action");
  bad(withSteps({ at_ms: 0, action: "set_mode" }), "set_mode");
  bad(withSteps({ at_ms: 0, action: "set_mode", value: "gamepad" }), "set_mode");
  bad(withSteps({ at_ms: 0, action: "set_deadzone" }), "set_deadzone");
  bad(withSteps({ at_ms: 0, action: "set_deadzone", value: {} }), "set_deadzone");
  bad(withSteps({ at_ms: 0, action: "set_deadzone", value: { edge: 1 } }), "edge");
  bad(withSteps({ at_ms: 0, action: "set_deadzone", value: { center: 0.9 } }), "center");
  bad(withSteps({ at_ms: 0, action: "set_deadzone", value: { range: 0.5 } }), "range");
  bad(withSteps({ at_ms: 0, action: "save", value: 1 }), "value");
  bad(withSteps({ at_ms: 0, action: "save", expect_reply: "maybe" }), "expect_reply");
});

test("validateSimScript accepts flash, revert and mark_valid and rejects bad uses", () => {
  ok(withSteps({ at_ms: 0, action: "flash" }, { at_ms: 1, action: "flash", version: "0.2.0", expect_reply: "ok" },
    { at_ms: 2, action: "revert" }, { at_ms: 3, action: "mark_valid", expect_reply: "error" }));
  bad(withSteps({ at_ms: 0, action: "flash", version: "" }), "version");
  bad(withSteps({ at_ms: 0, action: "flash", version: "x".repeat(32) }), "version");
  bad(withSteps({ at_ms: 0, action: "flash", version: 2 }), "version");
  bad(withSteps({ at_ms: 0, action: "revert", version: "1.0.0" }), "version");
  bad(withSteps({ at_ms: 0, action: "flash", value: 1 }), "value");
});

test("validateSimScript allows out-of-range deadzones only with expect_reply invalid", () => {
  ok(withSteps({ at_ms: 0, action: "set_deadzone", value: { center: 0.9, range: 0.5 }, expect_reply: "invalid" }));
  ok(withSteps({ at_ms: 0, action: "set_deadzone", value: { center: -1 }, expect_reply: "invalid" }));
  bad(withSteps({ at_ms: 0, action: "set_deadzone", value: { center: 0.9 }, expect_reply: "ok" }), "center");
  bad(withSteps({ at_ms: 0, action: "set_deadzone", value: { range: 0.5 }, expect_reply: "busy" }), "range");
  bad(withSteps({ at_ms: 0, action: "set_deadzone", value: { range: "wide" }, expect_reply: "invalid" }), "range");
  ok(withSteps({ at_ms: 0, action: "set_deadzone", value: { center: 0.5, range: 0.1 } }));
});

test("validateSimScript checks the firmware and gamepad expectations", () => {
  ok(withSteps({ at_ms: 0, expect: { fw_version: "0.2.0", xac_x: 0.5, xac_y: -1 } }));
  bad(withSteps({ at_ms: 0, expect: { fw_version: 2 } }), "fw_version");
  bad(withSteps({ at_ms: 0, expect: { xac_x: 1.5 } }), "xac_x");
  bad(withSteps({ at_ms: 0, expect: { xac_y: "up" } }), "xac_y");
  bad(withSteps({ at_ms: 0, expect: { center_max: 11 } }), "center_max");
});

test("validateSimScript limits at_ms, drift, noise and byte size", () => {
  ok(withSteps({ at_ms: 86400000, drift: 10 }));
  bad(withSteps({ at_ms: 86400001, drift: 1 }), "at_ms");
  bad(withSteps({ at_ms: 0, drift: 10.5 }), "drift");
  bad(withSteps({ at_ms: 0, noise: 1.01 }), "noise");
  // 256 KiB is counted in UTF-8 bytes, not characters.
  const big = { version: 1, name: "n", steps: [{ at_ms: 0, expect: { fw_version: "é".repeat(140000) } }] };
  assert.equal(P.validateSimScript(big).ok, false);
});

test("every scenario in sim/scenarios is accepted by validateSimScript", () => {
  const dir = path.join(__dirname, "..", "..", "sim", "scenarios");
  const files = fs.readdirSync(dir).filter((n) => n.endsWith(".json"));
  assert.ok(files.length >= 9, "expected the shipped scenarios, found " + files.length);
  for (const n of files) {
    const v = P.validateSimScript(JSON.parse(fs.readFileSync(path.join(dir, n), "utf8")));
    assert.ok(v.ok, n + ": " + v.errors.join(" "));
  }
});

test("validateSimScript checks expect steps", () => {
  bad(withSteps({ at_ms: 0, expect: {} }), "expect");
  bad(withSteps({ at_ms: 0, expect: "yes" }), "expect");
  bad(withSteps({ at_ms: 0, expect: { telepathy: true } }), "telepathy");
  bad(withSteps({ at_ms: 0, expect: { calibrated: "yes" } }), "calibrated");
  bad(withSteps({ at_ms: 0, expect: { center_max: -1 } }), "center_max");
  bad(withSteps({ at_ms: 0, expect: { mouse_moved: "sideways" } }), "mouse_moved");
  bad(withSteps({ at_ms: 0, expect: { clicked: "middle" } }), "clicked");
  bad(withSteps({ at_ms: 0, expect: { mode: "keyboard" } }), "mode");
  bad(withSteps({ at_ms: 0, expect: { fw_slot: "ota_2" } }), "fw_slot");
  bad(withSteps({ at_ms: 0, expect: { neutral: "yes" } }), "neutral");
});

test("validateSimScript caps the number of steps and the size", () => {
  const many = { version: 1, steps: Array.from({ length: 5001 }, (_, i) => ({ at_ms: i, pattern: "rest" })) };
  bad(many, "limit");
  const ok = { version: 1, steps: Array.from({ length: 5000 }, (_, i) => ({ at_ms: i, pattern: "rest" })) };
  assert.equal(P.validateSimScript(ok).ok, true);
  bad({ version: 1, name: "x".repeat(300000), steps: [] });
});

test("validateSimScript reports several errors and limits the list", () => {
  const steps = Array.from({ length: 50 }, () => ({ at_ms: 0, pattern: "nope" }));
  const r = P.validateSimScript({ version: 1, steps });
  assert.equal(r.ok, false);
  assert.equal(r.errors.length, 20);
});

// ---- createHidAccumulator ----

test("hid accumulator sums mouse motion and wraps the position", () => {
  const a = P.createHidAccumulator({ width: 100, height: 50 });
  assert.equal(a.posX, 50);
  assert.equal(a.posY, 25);
  a.apply({ type: "hid", mode: "mouse", dx: 30, dy: -10, wheel: 1, buttons: 0 });
  a.apply({ type: "hid", mode: "mouse", dx: 30, dy: -20, wheel: -3, buttons: 0 });
  assert.equal(a.totalDx, 60);
  assert.equal(a.totalDy, -30);
  assert.equal(a.wheel, -2);
  assert.equal(a.posX, 10); // 50 + 60 = 110, wrapped to 10
  assert.equal(a.posY, 45); // 25 - 30 = -5, wrapped to 45
  a.reset();
  assert.equal(a.totalDx, 0);
  assert.equal(a.posX, 50);
});

test("hid accumulator counts presses on the rising edge only", () => {
  const a = P.createHidAccumulator();
  const hid = (buttons) => a.apply({ type: "hid", mode: "mouse", dx: 0, dy: 0, wheel: 0, buttons });
  assert.deepEqual(hid(0).clicked, []);
  assert.deepEqual(hid(1).clicked, ["left"]);
  assert.deepEqual(hid(1).clicked, []); // still held
  assert.deepEqual(hid(0).clicked, []);
  assert.deepEqual(hid(3).clicked, ["left", "right"]);
  assert.deepEqual(hid(2).clicked, []);
  assert.deepEqual(hid(0).clicked, []);
  assert.deepEqual(hid(2).clicked, ["right"]);
  assert.equal(a.leftClicks, 2);
  assert.equal(a.rightClicks, 2);
  assert.equal(a.lastClick, "right");
});

test("hid accumulator keeps the latest xac value and ignores other messages", () => {
  const a = P.createHidAccumulator();
  a.apply({ type: "hid", mode: "xac", x: 0.25, y: -0.5 });
  a.apply({ type: "hid", mode: "xac", x: -0.1, y: 0.9 });
  assert.equal(a.mode, "xac");
  assert.equal(a.x, -0.1);
  assert.equal(a.y, 0.9);
  assert.equal(a.apply({ type: "sensor" }), null);
  assert.equal(a.apply(null), null);
});
