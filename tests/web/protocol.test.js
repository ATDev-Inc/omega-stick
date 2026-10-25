"use strict";
// Tests for components/omega_calibration/web/omega_protocol.js.
//
// Every expected byte here is derived from the C++ definitions, not from the
// JS under test:
//   - framing:  espp stream_frame.hpp (build-host/espp-v1.3.5/stream_frame or
//               managed_components/espp__stream_frame)
//   - payloads: components/omega_calibration/include/calibration_service.hpp
//   - discovery: espp dispatcher.hpp Dispatcher::describe()
// CRC values come either from published check values or from Node's own
// zlib.crc32 (an implementation independent of the JS under test).

const test = require("node:test");
const assert = require("node:assert/strict");
const zlib = require("node:zlib");
const path = require("node:path");

const P = require(path.join(__dirname, "..", "..", "components", "omega_calibration", "web", "omega_protocol.js"));

const bytes = (...b) => Uint8Array.from(b);
const ascii = (s) => Uint8Array.from(Buffer.from(s, "latin1"));
const u32le = (n) => bytes(n & 0xFF, (n >>> 8) & 0xFF, (n >>> 16) & 0xFF, (n >>> 24) & 0xFF);
const concat = (...parts) => Uint8Array.from(parts.flatMap((p) => Array.from(p)));
const f32le = (x) => { const b = Buffer.alloc(4); b.writeFloatLE(x, 0); return Uint8Array.from(b); };
const zcrc = (u8) => zlib.crc32(Buffer.from(u8)) >>> 0;

// Reference encoder written from stream_frame.hpp (not from the JS):
//   [54 4F][flags][module][type]{[corr u16] iff flags&2}[len u32][payload][crc32 u32]
// with crc32 over everything before it (header incl. optional corr + payload).
function refFrame(flags, module, type, payload, corr) {
  const head = [0x54, 0x4F, flags, module, type];
  if (corr !== undefined) head.push(corr & 0xFF, corr >> 8);
  const body = concat(bytes(...head), u32le(payload.length), payload);
  return concat(body, u32le(zcrc(body)));
}
const REQ = 0x10;   // make_flags(reply=false): version 1 in the high nibble
const REPLY = 0x11; // make_flags(reply=true)

// ---------------------------------------------------------------- crc32

test("crc32: zlib check value for '123456789' is 0xCBF43926", () => {
  assert.equal(P.crc32(ascii("123456789")), 0xCBF43926);
});

test("crc32: empty input is 0", () => {
  assert.equal(P.crc32(new Uint8Array(0)), 0);
});

test("crc32: known vectors 'a' and the pangram", () => {
  assert.equal(P.crc32(ascii("a")), 0xE8B7BE43);
  assert.equal(P.crc32(ascii("The quick brown fox jumps over the lazy dog")), 0x414FA339);
});

test("crc32: agrees with node zlib.crc32 on a 256-byte ramp and works on subarray views", () => {
  const ramp = Uint8Array.from({ length: 256 }, (_, i) => i);
  assert.equal(P.crc32(ramp), zcrc(ramp));
  assert.equal(P.crc32(ramp.subarray(10, 20)), zcrc(ramp.subarray(10, 20)));
});

// ---------------------------------------------------------------- constants

test("constants match stream_frame.hpp and calibration_service.hpp", () => {
  assert.equal(P.HEADER_SIZE, 9);      // kHeaderSize
  assert.equal(P.CRC_SIZE, 4);         // kCrcSize
  assert.equal(P.MAX_PAYLOAD, 4096);   // kMaxPayloadSize
  assert.equal(P.FLAGS_REQUEST, 0x10); // make_flags(false)
  assert.equal(P.FLAGS_REPLY_BIT, 0x01); // kFlagReply
  assert.equal(P.FLAG_CORRELATION, 0x02); // kFlagCorrelation
  assert.equal(P.MODULE_CALIBRATION, 5);  // CalibrationService::kModule
  assert.equal(P.MODULE_DISCOVERY, 0xFF); // Dispatcher::kDiscoveryModule
  assert.equal(P.T_LIST_MODULES, 0x00);   // Dispatcher::Discovery::ListModules
  assert.deepEqual(P.TYPE, {
    GET_STATE: 0x50, SET_MANUAL_OFFSET: 0x51, SET_DEADZONE_OVERRIDE: 0x52,
    SET_NUDGE_SETTINGS: 0x53, SET_INPUT_MODE: 0x54, SAVE_TO_FLASH: 0x55,
    START_CALIBRATION: 0x56, RESTART: 0x57, OTA_BEGIN: 0x58, OTA_DATA: 0x59,
    OTA_FINISH: 0x5A, OTA_ABORT: 0x5B, GET_FW_INFO: 0x5C, MARK_VALID: 0x5D,
    REVERT: 0x5E, GET_DIAGNOSTICS: 0x5F, SET_NUDGE_ENABLED: 0x60, STATE: 0xD0, CALIBRATION_RESULT: 0xD1, FW_INFO: 0xD2, DIAGNOSTICS: 0xD3,
    OK: 0xDF, ERROR: 0xFE,
  });
  assert.deepEqual(P.INPUT_MODE, { MOUSE: 0, XAC: 1 });
});

// ---------------------------------------------------------------- buildFrame

test("buildFrame: GET_STATE (empty payload) is exactly 13 bytes", () => {
  // pos 0-1  54 4F        magic 0x4F54 little-endian
  // pos 2    10           flags: version 1 << 4, reply bit clear, no correlation
  // pos 3    05           module 5 (calibration)
  // pos 4    50           type GET_STATE
  // pos 5-8  00 00 00 00  len u32 LE = 0
  // pos 9-12 23 16 0A 36  crc32 of pos 0-8 = 0x360A1623, little-endian
  const expected = bytes(0x54, 0x4F, 0x10, 0x05, 0x50, 0, 0, 0, 0, 0x23, 0x16, 0x0A, 0x36);
  assert.deepEqual(P.buildFrame(P.MODULE_CALIBRATION, P.TYPE.GET_STATE), expected);
  assert.deepEqual(P.buildFrame(P.MODULE_CALIBRATION, P.TYPE.GET_STATE, null), expected);
  assert.equal(zcrc(expected.subarray(0, 9)), 0x360A1623);
});

test("buildFrame: SET_INPUT_MODE(XAC) with a 1-byte payload is 14 bytes", () => {
  // pos 0-8  54 4F 10 05 54 01 00 00 00  (type 0x54, len = 1)
  // pos 9    01                          payload: mode 1 = XAC
  // pos 10-13 D9 CF A5 A1                crc32 of pos 0-9 = 0xA1A5CFD9, LE
  const expected = bytes(0x54, 0x4F, 0x10, 0x05, 0x54, 1, 0, 0, 0, 0x01, 0xD9, 0xCF, 0xA5, 0xA1);
  assert.deepEqual(P.buildFrame(5, 0x54, bytes(1)), expected);
  assert.equal(zcrc(expected.subarray(0, 10)), 0xA1A5CFD9);
});

test("buildFrame: SET_MANUAL_OFFSET(1.0, -2.0) with an 8-byte payload", () => {
  // payload: f32 LE 1.0 = 00 00 80 3F, f32 LE -2.0 = 00 00 00 C0
  // len = 8; crc32 over pos 0-16 = 0xE4102B2C -> 2C 2B 10 E4
  const expected = bytes(0x54, 0x4F, 0x10, 0x05, 0x51, 8, 0, 0, 0,
                         0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0xC0,
                         0x2C, 0x2B, 0x10, 0xE4);
  assert.deepEqual(P.buildFrame(5, 0x51, P.encodeSetManualOffset(1.0, -2.0)), expected);
});

test("buildFrame: CRC covers magic + header + payload, exactly as stream_frame.hpp", () => {
  const frame = P.buildFrame(5, 0x51, P.encodeSetManualOffset(0.25, 0.5));
  const n = frame.length;
  const stored = new DataView(frame.buffer).getUint32(n - 4, true);
  assert.equal(stored, zcrc(frame.subarray(0, n - 4)));       // whole header + payload
  assert.notEqual(stored, zcrc(frame.subarray(9, n - 4)));    // not payload only
  assert.notEqual(stored, zcrc(frame.subarray(2, n - 4)));    // not skipping the magic
  assert.deepEqual(frame, refFrame(REQ, 5, 0x51, frame.subarray(9, n - 4)));
});

test("buildFrame: payload limit is 4096 bytes", () => {
  assert.equal(P.buildFrame(5, 0x51, new Uint8Array(4096)).length, 9 + 4096 + 4);
  assert.throws(() => P.buildFrame(5, 0x51, new Uint8Array(4097)), /exceeds 4096/);
});

// ---------------------------------------------------------------- encoders
// calibration_service.hpp: SET_MANUAL_OFFSET f32 x, f32 y; SET_DEADZONE_OVERRIDE
// u8 flags (bit0 center, bit1 range) + f32 center + f32 range (fixed 9 bytes);
// SET_NUDGE_SETTINGS f32 enter, f32 exit, u32 time_ms; SET_INPUT_MODE u8.

test("encodeSetDeadzoneOverride: flag bits and fixed 9-byte length", () => {
  assert.deepEqual(P.encodeSetDeadzoneOverride(true, 0.5, true, 0.25),
                   concat(bytes(0x03), f32le(0.5), f32le(0.25)));
  assert.deepEqual(P.encodeSetDeadzoneOverride(true, 0.5, false, 0.25),
                   concat(bytes(0x01), f32le(0.5), f32le(0)));
  assert.deepEqual(P.encodeSetDeadzoneOverride(false, 0.5, true, 0.25),
                   concat(bytes(0x02), f32le(0), f32le(0.25)));
  assert.deepEqual(P.encodeSetDeadzoneOverride(false, 0.5, false, 0.25),
                   concat(bytes(0x00), f32le(0), f32le(0)));
});

test("encodeSetNudgeSettings: f32 enter, f32 exit, u32 ms, little-endian", () => {
  assert.deepEqual(P.encodeSetNudgeSettings(0.5, 0.125, 300),
                   concat(f32le(0.5), f32le(0.125), u32le(300)));
});

test("encodeSetInputMode: one byte, 0 = mouse, 1 = XAC", () => {
  assert.deepEqual(P.encodeSetInputMode(P.INPUT_MODE.MOUSE), bytes(0));
  assert.deepEqual(P.encodeSetInputMode(P.INPUT_MODE.XAC), bytes(1));
});

test("encodeSetNudgeEnabled: one byte, 0 = off, 1 = on", () => {
  assert.deepEqual(P.encodeSetNudgeEnabled(false), bytes(0));
  assert.deepEqual(P.encodeSetNudgeEnabled(true), bytes(1));
  assert.deepEqual(P.buildFrame(5, P.TYPE.SET_NUDGE_ENABLED, P.encodeSetNudgeEnabled(false)).subarray(0, 10),
                   concat(ascii("TO"), bytes(0x10, 5, 0x60), u32le(1), bytes(0)));
});

test("encodeSetManualOffset: two f32 LE, 8 bytes", () => {
  assert.deepEqual(P.encodeSetManualOffset(-0.5, 4), concat(f32le(-0.5), f32le(4)));
});

// ---------------------------------------------------------------- FrameParser

const okReply = refFrame(REPLY, 5, 0xDF, u32le(0));      // 17 bytes
const emptyReply = refFrame(REPLY, 5, 0xD0, bytes());    // 13 bytes
const noText = (r) => assert.equal(r.text.length, 0);

test("parser: whole frame in one chunk", () => {
  const r = new P.FrameParser().feed(okReply);
  assert.equal(r.frames.length, 1);
  const f = r.frames[0];
  assert.equal(f.flags, REPLY);
  assert.equal(f.reply, true);
  assert.equal(f.correlation, null);
  assert.equal(f.module, 5);
  assert.equal(f.type, 0xDF);
  assert.deepEqual(f.payload, u32le(0));
  noText(r);
});

test("parser: frame split across chunks, down to single bytes", () => {
  const p = new P.FrameParser();
  const cut = 6;
  assert.equal(p.feed(okReply.subarray(0, cut)).frames.length, 0);
  const r = p.feed(okReply.subarray(cut));
  assert.equal(r.frames.length, 1);
  assert.equal(r.frames[0].type, 0xDF);

  const q = new P.FrameParser();
  const out = [];
  for (const b of okReply) out.push(...q.feed(bytes(b)).frames);
  assert.equal(out.length, 1);
  assert.deepEqual(out[0].payload, u32le(0));
});

test("parser: magic split between chunks (lone 0x54 is kept)", () => {
  const p = new P.FrameParser();
  assert.equal(p.feed(okReply.subarray(0, 1)).frames.length, 0);
  assert.equal(p.feed(okReply.subarray(1)).frames.length, 1);
});

test("parser: garbage before the magic is returned as console text", () => {
  // 'hi' + 0x54 not followed by 0x4F ('T' then 'x') + newline: none is a magic.
  const garbage = ascii("hi Tx\n");
  const r = new P.FrameParser().feed(concat(garbage, okReply));
  assert.equal(r.frames.length, 1);
  assert.deepEqual(r.text, garbage);
});

test("parser: text between two frames is separated from both", () => {
  const r = new P.FrameParser().feed(concat(emptyReply, ascii("log line\n"), okReply));
  assert.deepEqual(r.frames.map((f) => f.type), [0xD0, 0xDF]);
  assert.deepEqual(r.text, ascii("log line\n"));
});

test("parser: bad CRC drops that frame and parsing resumes at the next one", () => {
  const bad = Uint8Array.from(okReply);
  bad[10] ^= 0xFF; // corrupt a payload byte
  const r = new P.FrameParser().feed(concat(bad, emptyReply));
  assert.equal(r.frames.length, 1);
  assert.equal(r.frames[0].type, 0xD0);
  assert.deepEqual(r.text, bad); // the discarded bytes come back as text
});

test("parser: corrupting a header byte (module) is caught because the CRC covers it", () => {
  const bad = Uint8Array.from(okReply);
  bad[3] = 6;
  const r = new P.FrameParser().feed(bad);
  assert.equal(r.frames.length, 0);
});

test("parser: truncated frame followed by a valid frame still yields the valid one", () => {
  // Header says len = 4 but only 2 payload bytes arrive before the next frame
  // starts. The parser sees 24 bytes, CRC fails, skips one byte and resyncs.
  const truncated = okReply.subarray(0, 9 + 2);
  const r = new P.FrameParser().feed(concat(truncated, emptyReply));
  assert.equal(r.frames.length, 1);
  assert.equal(r.frames[0].type, 0xD0);
  assert.deepEqual(r.text, truncated);
});

test("parser: an incomplete tail is held, then flushed as text when stale", () => {
  const p = new P.FrameParser();
  const r = p.feed(okReply.subarray(0, 12));
  assert.equal(r.frames.length, 0);
  noText(r);
  assert.equal(p.flushStale(10 * 60 * 1000), null); // fresh: keep waiting
  assert.deepEqual(p.flushStale(0), okReply.subarray(0, 12));
  assert.equal(p.flushStale(0), null); // buffer now empty
});

test("parser: two frames in one chunk come out in order", () => {
  const r = new P.FrameParser().feed(concat(emptyReply, okReply));
  assert.deepEqual(r.frames.map((f) => f.type), [0xD0, 0xDF]);
  noText(r);
});

test("parser: oversized length (> 4096) is rejected and resyncs", () => {
  const huge = concat(bytes(0x54, 0x4F, REPLY, 5, 0xDF), u32le(4097));
  const r = new P.FrameParser().feed(concat(huge, emptyReply));
  assert.equal(r.frames.length, 1);
  assert.equal(r.frames[0].type, 0xD0);
});

test("parser: correlation-id header (flags bit1) is 11 bytes and CRC-covered", () => {
  const withCorr = refFrame(REPLY | 0x02, 5, 0xDF, u32le(0), 0x1234);
  assert.equal(withCorr.length, 11 + 4 + 4);
  const r = new P.FrameParser().feed(withCorr);
  assert.equal(r.frames.length, 1);
  assert.equal(r.frames[0].correlation, 0x1234);
  assert.deepEqual(r.frames[0].payload, u32le(0));
});

test("parser: frames built by buildFrame parse back (request direction, reply flag clear)", () => {
  const r = new P.FrameParser().feed(P.buildFrame(5, 0x53, P.encodeSetNudgeSettings(0.5, 0.25, 100)));
  assert.equal(r.frames.length, 1);
  assert.equal(r.frames[0].reply, false);
  assert.equal(r.frames[0].flags, 0x10);
  assert.equal(r.frames[0].payload.length, 12);
});

test("parser: reset() discards buffered bytes", () => {
  const p = new P.FrameParser();
  p.feed(okReply.subarray(0, 12));
  p.reset();
  assert.equal(p.feed(okReply.subarray(12)).frames.length, 0);
});

// ---------------------------------------------------------------- parseErrorReply

test("parseErrorReply: u32 code + utf8 message", () => {
  const payload = concat(u32le(5), Uint8Array.from(Buffer.from("flash busy é", "utf8")));
  assert.deepEqual(P.parseErrorReply(payload), { code: 5, message: "flash busy é" });
});

test("parseErrorReply: code only gives an empty message", () => {
  assert.deepEqual(P.parseErrorReply(u32le(0x01020304)), { code: 0x01020304, message: "" });
});

test("parseErrorReply: short payload is reported as malformed", () => {
  assert.deepEqual(P.parseErrorReply(bytes(1, 2, 3)), { code: 0, message: "(malformed error reply)" });
  assert.deepEqual(P.parseErrorReply(new Uint8Array(0)), { code: 0, message: "(malformed error reply)" });
});

test("parseErrorReply: works on a subarray view with a non-zero byteOffset", () => {
  // The console calls this on frame.payload.subarray(1) for CALIBRATION_RESULT.
  const whole = concat(bytes(0), u32le(9), ascii("nope"));
  assert.deepEqual(P.parseErrorReply(whole.subarray(1)), { code: 9, message: "nope" });
});

// ---------------------------------------------------------------- parseDiscovery
// Dispatcher::describe(): [version u8 = 1][reserved u8 = 0][device str][fw str]
// [module_count u8] then per module [id u8][name str][app str][desc str],
// where str = [len u8][bytes].

const str = (s) => { const b = Uint8Array.from(Buffer.from(s, "utf8")); return concat(bytes(b.length), b); };

test("parseDiscovery: device info and a module list", () => {
  const payload = concat(
    bytes(1, 0), str("Omega Stick"), str("v1.2"),
    bytes(2),
    bytes(4), str("Core dump"), str("coredump_console.html"), str("Fetch crash dumps"),
    bytes(5), str("Calibration"), str("calibration_console.html"), str(""));
  assert.deepEqual(P.parseDiscovery(payload), {
    version: 1,
    device: "Omega Stick",
    fw: "v1.2",
    modules: [
      { id: 4, name: "Core dump", app: "coredump_console.html", desc: "Fetch crash dumps" },
      { id: 5, name: "Calibration", app: "calibration_console.html", desc: "" },
    ],
  });
});

test("parseDiscovery: v2 records carry protocol id and version", () => {
  const payload = concat(
    bytes(2, 0), str("Omega Stick"), str("v1.2"),
    bytes(1),
    bytes(5), str("Calibration"), str("calibration_console.html"), str(""),
    str(P.PROTOCOL_ID), bytes(1, 0));
  const info = P.parseDiscovery(payload);
  assert.equal(P.PROTOCOL_ID, "omega.calibration");
  assert.deepEqual(info.modules, [
    { id: 5, name: "Calibration", app: "calibration_console.html", desc: "",
      protocol: "omega.calibration", protocolVersion: 1 },
  ]);
});

test("parseDiscovery: empty device strings and zero modules", () => {
  assert.deepEqual(P.parseDiscovery(bytes(1, 0, 0, 0, 0)),
                   { version: 1, device: "", fw: "", modules: [] });
});

test("parseDiscovery: truncated payloads throw", () => {
  const full = concat(bytes(1, 0), str("D"), str("F"), bytes(1), bytes(5), str("Cal"), str("a"), str("d"));
  for (let n = 0; n < full.length; n++) {
    assert.throws(() => P.parseDiscovery(full.subarray(0, n)), /truncated discovery payload/, "cut at " + n);
  }
  assert.doesNotThrow(() => P.parseDiscovery(full));
});

// ---------------------------------------------------------------- decodeState
// calibration_service.hpp build_state_payload(): 80 bytes, little-endian
// (79 from firmware without the nudge on/off switch):
//   off  0..23  f32 center_x, center_y, center_noise_radius, center_deadzone,
//                   center_manual_offset_x, center_manual_offset_y
//   off 24..55  f32 range_min_x, range_max_x, range_min_y, range_max_y,
//                   range_usable_radius, range_reliable_max_radius,
//                   range_normalized_max_radius, range_deadzone
//   off 56      u8  has_manual_center_deadzone
//   off 57..60  f32 manual_center_deadzone
//   off 61      u8  has_manual_range_deadzone
//   off 62..65  f32 manual_range_deadzone
//   off 66..69  f32 nudge_enter_threshold
//   off 70..73  f32 nudge_exit_threshold
//   off 74..77  u32 nudge_time_ms
//   off 78      u8  input_mode (0 mouse, 1 XAC)
//   off 79      u8  nudge_enabled (0/1; absent in the 79-byte form)

// `nudge` undefined builds the older 79-byte form.
function statePayload({ hasCenter, hasRange, mode, nudge }) {
  return concat(
    // center block (all values exactly representable as f32)
    f32le(0.5), f32le(-0.5), f32le(0.015625), f32le(0.03125), f32le(0.25), f32le(-0.25),
    // range block
    f32le(-1), f32le(1.5), f32le(-2), f32le(2.5),
    f32le(0.75), f32le(0.875), f32le(0.9375), f32le(0.0625),
    // manual deadzones
    bytes(hasCenter ? 1 : 0), f32le(0.125),
    bytes(hasRange ? 1 : 0), f32le(0.375),
    // nudge + mode
    f32le(0.2), f32le(0.1), u32le(250),
    bytes(mode),
    nudge === undefined ? bytes() : bytes(nudge));
}

test("decodeState: 79-byte layout, both manual deadzones present, XAC mode", () => {
  const payload = statePayload({ hasCenter: true, hasRange: true, mode: 1 });
  assert.equal(payload.length, 79);
  const s = P.decodeState(payload);
  assert.equal(s.centerX, 0.5);
  assert.equal(s.centerY, -0.5);
  assert.equal(s.centerNoise, 0.015625);
  assert.equal(s.centerDeadzoneAuto, 0.03125);
  assert.equal(s.manualOffsetX, 0.25);
  assert.equal(s.manualOffsetY, -0.25);
  assert.equal(s.rangeMinX, -1);
  assert.equal(s.rangeMaxX, 1.5);
  assert.equal(s.rangeMinY, -2);
  assert.equal(s.rangeMaxY, 2.5);
  assert.equal(s.usableRadius, 0.75);
  assert.equal(s.reliableMaxRadius, 0.875);
  assert.equal(s.normalizedMaxRadius, 0.9375);
  assert.equal(s.rangeDeadzoneAuto, 0.0625);
  assert.equal(s.manualCenterDeadzone, 0.125);
  assert.equal(s.manualRangeDeadzone, 0.375);
  assert.equal(s.nudgeEnterThreshold, Math.fround(0.2));
  assert.equal(s.nudgeExitThreshold, Math.fround(0.1));
  assert.equal(s.nudgeTimeMs, 250);
  assert.equal(s.inputMode, 1);
});

test("decodeState: has_* = 0 yields null overrides even though the f32 is present", () => {
  const s = P.decodeState(statePayload({ hasCenter: false, hasRange: false, mode: 0 }));
  assert.equal(s.manualCenterDeadzone, null);
  assert.equal(s.manualRangeDeadzone, null);
  assert.equal(s.inputMode, 0);
});

test("decodeState: only one override present", () => {
  const s = P.decodeState(statePayload({ hasCenter: false, hasRange: true, mode: 0 }));
  assert.equal(s.manualCenterDeadzone, null);
  assert.equal(s.manualRangeDeadzone, 0.375);
});

test("decodeState: short payload throws, longer payload is tolerated", () => {
  const ok = statePayload({ hasCenter: true, hasRange: true, mode: 0 });
  assert.throws(() => P.decodeState(ok.subarray(0, 78)), /malformed STATE reply \(78 B, expected 79 or 80\)/);
  const longer = P.decodeState(concat(ok, bytes(1, 0xAA)));
  assert.equal(longer.nudgeTimeMs, 250);
  assert.equal(longer.nudgeEnabled, true);
});

test("decodeState: 80-byte layout carries nudge_enabled as the last byte", () => {
  const on = statePayload({ hasCenter: false, hasRange: false, mode: 0, nudge: 1 });
  assert.equal(on.length, 80);
  assert.equal(P.decodeState(on).nudgeEnabled, true);
  assert.equal(P.decodeState(on).inputMode, 0);
  const off = statePayload({ hasCenter: false, hasRange: false, mode: 1, nudge: 0 });
  assert.equal(P.decodeState(off).nudgeEnabled, false);
  assert.equal(P.decodeState(off).inputMode, 1);
  assert.equal(P.decodeState(off).nudgeTimeMs, 250);
});

test("decodeState: 79-byte STATE from older firmware decodes nudgeEnabled as null", () => {
  const s = P.decodeState(statePayload({ hasCenter: true, hasRange: true, mode: 1 }));
  assert.equal(s.nudgeEnabled, null);
  assert.equal(s.inputMode, 1);
});

// ---------------------------------------------------------------- nudgeControls
test("nudgeControls: checkbox follows the device; off greys out the tuning but keeps it", () => {
  const live = { connected: true, busy: false, haveState: true };
  assert.deepEqual(P.nudgeControls({ ...live, nudgeEnabled: true }),
                   { checked: true, toggleDisabled: false, tuningDisabled: false, applyDisabled: false, note: "" });
  assert.deepEqual(P.nudgeControls({ ...live, nudgeEnabled: false }),
                   { checked: false, toggleDisabled: false, tuningDisabled: true, applyDisabled: true, note: "" });
});

test("nudgeControls: firmware without the switch shows it checked, disabled, with a note", () => {
  const c = P.nudgeControls({ connected: true, busy: false, haveState: true, nudgeEnabled: null });
  assert.equal(c.checked, true);
  assert.equal(c.toggleDisabled, true);
  assert.equal(c.tuningDisabled, false);
  assert.equal(c.applyDisabled, false);
  assert.equal(c.note, P.NUDGE_SWITCH_UNSUPPORTED_NOTE);
});

test("nudgeControls: nothing to toggle while disconnected, busy or before the first STATE", () => {
  for (const ctx of [{ connected: false, busy: false, haveState: true },
                     { connected: true, busy: true, haveState: true },
                     { connected: true, busy: false, haveState: false }]) {
    const c = P.nudgeControls({ ...ctx, nudgeEnabled: ctx.haveState ? true : null });
    assert.equal(c.toggleDisabled, true, JSON.stringify(ctx));
    assert.equal(c.applyDisabled, true, JSON.stringify(ctx));
    assert.equal(c.note, "", JSON.stringify(ctx));
  }
  // Off stays visibly off while busy (the values are kept, just greyed).
  const busyOff = P.nudgeControls({ connected: true, busy: true, haveState: true, nudgeEnabled: false });
  assert.equal(busyOff.checked, false);
  assert.equal(busyOff.tuningDisabled, true);
});

test("decodeState: honours byteOffset of a parsed frame payload", () => {
  const frame = refFrame(REPLY, 5, 0xD0, statePayload({ hasCenter: true, hasRange: false, mode: 1 }));
  const r = new P.FrameParser().feed(concat(ascii("xx"), frame));
  assert.equal(r.frames.length, 1);
  assert.equal(r.frames[0].payload.length, 79);
  assert.equal(P.decodeState(r.frames[0].payload).manualCenterDeadzone, 0.125);
  // And a subarray view with a non-zero offset into a bigger buffer:
  const padded = concat(bytes(9, 9, 9), statePayload({ hasCenter: true, hasRange: true, mode: 1 }));
  assert.equal(P.decodeState(padded.subarray(3)).centerX, 0.5);
});

// ------------------------------------------------- selectCalibrationModule
function v2Info(...mods) {
  return { version: 2, device: "d", fw: "f", modules: mods.map((m) => ({ name: "n", app: "a", desc: "", ...m })) };
}

test("selectCalibrationModule: v2 service configured on module 7", () => {
  const info = v2Info({ id: 4, protocol: "omega.ota", protocolVersion: 1 },
                      { id: 7, protocol: P.PROTOCOL_ID, protocolVersion: 1 });
  assert.deepEqual(P.selectCalibrationModule(info), { id: 7, how: "protocol", versionMismatch: false });
});

test("selectCalibrationModule: v2 service on the default module 5", () => {
  const info = v2Info({ id: 5, protocol: P.PROTOCOL_ID, protocolVersion: 1 });
  assert.deepEqual(P.selectCalibrationModule(info), { id: 5, how: "protocol", versionMismatch: false });
});

test("selectCalibrationModule: v1 falls back to the default id", () => {
  const info = { version: 1, device: "d", fw: "f", modules: [{ id: 5, name: "cal", app: "a", desc: "" }] };
  assert.deepEqual(P.selectCalibrationModule(info), { id: 5, how: "legacy-id", versionMismatch: false });
  info.modules[0].id = 7; // v1 cannot identify a moved service
  assert.deepEqual(P.selectCalibrationModule(info), { id: 5, how: "missing", versionMismatch: false });
});

test("selectCalibrationModule: v2 without the protocol is missing, even at id 5", () => {
  const info = v2Info({ id: 5, protocol: "omega.other", protocolVersion: 1 });
  assert.deepEqual(P.selectCalibrationModule(info), { id: 5, how: "missing", versionMismatch: false });
  assert.deepEqual(P.selectCalibrationModule(v2Info()), { id: 5, how: "missing", versionMismatch: false });
});

test("selectCalibrationModule: protocol version mismatch is flagged", () => {
  const info = v2Info({ id: 7, protocol: P.PROTOCOL_ID, protocolVersion: 2 });
  assert.deepEqual(P.selectCalibrationModule(info), { id: 7, how: "protocol", versionMismatch: true });
});

test("usbUpdatesUnavailable hides the update controls only for function_not_supported", () => {
  // The build has updates over USB turned off: GET_FW_INFO is refused.
  assert.equal(
    P.usbUpdatesUnavailable(new Error("device error (code 88): GET_FW_INFO: Function not implemented")),
    true);
  assert.equal(P.usbUpdatesUnavailable("ota not wired on this build"), true);
  // Transient failures keep the controls.
  assert.equal(P.usbUpdatesUnavailable(new Error("GET_FW_INFO timed out")), false);
  assert.equal(P.usbUpdatesUnavailable(new Error("transport lost")), false);
  assert.equal(P.usbUpdatesUnavailable(new Error("not connected")), false);
  assert.equal(P.usbUpdatesUnavailable(new Error("something else broke")), false);
  // Success (no error) keeps the controls.
  assert.equal(P.usbUpdatesUnavailable(null), false);
  assert.equal(P.usbUpdatesUnavailable(undefined), false);
});

test("the no-updates note names the first-flash page and the erase", () => {
  assert.match(P.NO_USB_UPDATES_NOTE, /first-flash page/);
  assert.match(P.NO_USB_UPDATES_NOTE, /calibration and the saved settings|saved calibration and settings/);
  assert.equal(new URL(P.FIRST_FLASH_URL).pathname, "/omega-stick/flash.html");
});

test("decideOutcomeNoUpdates reports an installed image, not a failure or a rollback", () => {
  const o = P.decideOutcomeNoUpdates("0.1.0");
  assert.equal(o.state, "installed_no_updates");
  assert.match(o.message, /installed and running \(0\.1\.0\)/);
  assert.match(o.message, /does not take further updates over USB/);
  assert.doesNotMatch(o.message, /roll|not confirmed|did not come back/i);
  assert.equal(P.decideOutcomeNoUpdates("").state, "installed_no_updates");
});

// ---- DIAGNOSTICS calibration_state (D26) -----------------------------------
// calibration_service.hpp: ... u8 reset_count, N x reset entry, then u8
// calibration_state (0 usable, 1 needs calibration). Older firmware ends the
// payload after the reset list.

const diagHeader = (resetCount) => concat(bytes(0, 0), u32le(0), u32le(7), bytes(resetCount));
const resetEntry = (boot, name) => concat(u32le(boot), bytes(1, 0, name.length), ascii(name));

test("decodeDiagnostics: an older payload without calibration_state decodes as null", () => {
  const d = P.decodeDiagnostics(concat(diagHeader(1), resetEntry(7, "power-on")));
  assert.equal(d.calibrationState, null);
  assert.equal(d.resets.length, 1);
  assert.equal(d.resets[0].name, "power-on");
  assert.equal(P.decodeDiagnostics(diagHeader(0)).calibrationState, null); // the 11-byte minimum
});

test("decodeDiagnostics: reads the trailing calibration_state after the reset list", () => {
  const needs = P.decodeDiagnostics(concat(diagHeader(1), resetEntry(7, "power-on"), bytes(1)));
  assert.equal(needs.calibrationState, P.CALIBRATION_NEEDED);
  assert.equal(needs.resets[0].name, "power-on");
  assert.equal(needs.bootCount, 7);
  assert.equal(P.decodeDiagnostics(concat(diagHeader(0), bytes(0))).calibrationState, P.CALIBRATION_USABLE);
  // Bytes a later firmware might append after it are ignored.
  assert.equal(P.decodeDiagnostics(concat(diagHeader(0), bytes(1, 0xAA, 0xBB))).calibrationState, 1);
});

test("calibrationNotice: shown only when the device says it needs calibration", () => {
  assert.equal(P.CALIBRATION_USABLE, 0);
  assert.equal(P.CALIBRATION_NEEDED, 1);
  const notice = P.calibrationNotice({ calibrationState: 1 });
  assert.equal(notice, P.CALIBRATION_NEEDED_NOTICE);
  assert.match(notice, /no usable calibration/);
  assert.match(notice, /held neutral/);
  assert.match(notice, /Re-run calibration/);
  assert.doesNotMatch(notice, /\u2014/); // no em dashes in user-facing text
  assert.equal(P.calibrationNotice({ calibrationState: 0 }), "");
  assert.equal(P.calibrationNotice({ calibrationState: null }), ""); // older firmware: no claim either way
  assert.equal(P.calibrationNotice(null), "");
  assert.equal(P.calibrationNotice(undefined), "");
  // An unknown non-zero code is treated as "not usable", like the sensor codes.
  assert.equal(P.calibrationNotice({ calibrationState: 7 }), P.CALIBRATION_NEEDED_NOTICE);
});

test("describeCalibrationState names each state", () => {
  assert.equal(P.describeCalibrationState(0), "usable");
  assert.match(P.describeCalibrationState(1), /needs calibration/);
  assert.match(P.describeCalibrationState(null), /older firmware/);
  assert.equal(P.describeCalibrationState(5), "code 5");
});
