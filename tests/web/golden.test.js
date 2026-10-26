"use strict";
// Cross-language golden test: tests/protocol/golden_frames.json is generated
// by tests/host/test_golden_frames.cpp from the real C++ service and parser.
// Here the JS side must produce the same request bytes and decode the same
// reply fields. Regenerate the file from C++ (OMEGA_WRITE_GOLDEN=1), never by
// hand.

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const P = require(path.join(__dirname, "..", "..", "components", "omega_calibration", "web", "omega_protocol.js"));
const golden = JSON.parse(fs.readFileSync(path.join(__dirname, "..", "protocol", "golden_frames.json"), "utf8"));

const fromHex = (h) => Uint8Array.from(Buffer.from(h, "hex"));
const toHex = (u8) => Buffer.from(u8).toString("hex");

// Payload encoders per request name; GET_STATE, SAVE_TO_FLASH, START_CALIBRATION
// and LIST_MODULES carry no payload.
const ENCODERS = {
  SET_MANUAL_OFFSET: (a) => P.encodeSetManualOffset(a.x, a.y),
  SET_DEADZONE_OVERRIDE_BOTH: (a) => P.encodeSetDeadzoneOverride(a.centerOn, a.centerVal, a.rangeOn, a.rangeVal),
  SET_DEADZONE_OVERRIDE_RANGE_ONLY: (a) => P.encodeSetDeadzoneOverride(a.centerOn, a.centerVal, a.rangeOn, a.rangeVal),
  SET_NUDGE_SETTINGS: (a) => P.encodeSetNudgeSettings(a.enter, a.exit, a.timeMs),
  SET_INPUT_MODE_MOUSE: (a) => P.encodeSetInputMode(a.mode),
  SET_INPUT_MODE_INVALID: (a) => P.encodeSetInputMode(a.mode),
  SET_NUDGE_ENABLED_OFF: (a) => P.encodeSetNudgeEnabled(a.enabled),
};

// C++ prints each f32 as the shortest decimal that round-trips as a float, so
// compare after rounding the JS double to float32 as well.
function normalise(value) {
  if (typeof value === "number") return Math.fround(value);
  if (Array.isArray(value)) return value.map(normalise);
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.entries(value).map(([k, v]) => [k, normalise(v)]));
  }
  return value;
}

function decodeReply(frame) {
  switch (frame.type) {
    case P.TYPE.STATE: return P.decodeState(frame.payload);
    case P.TYPE.DIAGNOSTICS: return P.decodeDiagnostics(frame.payload);
    case P.TYPE.OK: return { value: P.readU32(frame.payload, 0) };
    case P.TYPE.ERROR: return P.parseErrorReply(frame.payload);
    case P.TYPE.CALIBRATION_RESULT: return { success: frame.payload[0] === 1 };
    case P.T_LIST_MODULES: return P.parseDiscovery(frame.payload);
    default: throw new Error("no decoder for type " + frame.type);
  }
}

test("golden file has the expected shape", () => {
  assert.ok(golden.requests.length >= 10);
  assert.ok(golden.replies.length >= 10);
  for (const r of golden.requests) assert.equal(typeof r.frame_hex, "string", r.name);
});

for (const req of golden.requests) {
  test("golden request " + req.name + ": JS encoders produce the C++ bytes", () => {
    const payload = ENCODERS[req.name] ? ENCODERS[req.name](req.args) : new Uint8Array(0);
    assert.equal(toHex(payload), req.payload_hex);
    assert.equal(toHex(P.buildFrame(req.module, req.type, payload)), req.frame_hex);
  });
}

for (const rep of golden.replies) {
  test("golden reply " + rep.name + ": JS parser and decoder match the C++ fields", () => {
    const parser = new P.FrameParser();
    const { frames, text } = parser.feed(fromHex(rep.frame_hex));
    assert.equal(text.length, 0);
    assert.equal(frames.length, 1);
    const frame = frames[0];
    assert.equal(frame.reply, true);
    assert.equal(frame.module, rep.module);
    assert.equal(frame.type, rep.type);
    assert.deepEqual(normalise(decodeReply(frame)), normalise(rep.fields));
  });
}
