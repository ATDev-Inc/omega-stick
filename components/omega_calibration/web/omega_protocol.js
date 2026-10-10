// omega_protocol.js: pure (DOM-free) wire-protocol code for the Omega
// calibration console. Framing is the espp stream_frame "OT" v1 format, the
// payload layouts are in components/omega_calibration/include/calibration_service.hpp.
//
// Classic script (not an ES module): loads from file:// and also works under
// Node via require(). A verbatim copy lives inside calibration_console.html
// between the BEGIN/END markers; tests/web/sync.test.js fails if they
// drift, so edit this file and then copy it into the HTML block.
(function () {
  "use strict";

  const HEADER_SIZE = 9, CRC_SIZE = 4;
  const MAX_PAYLOAD = 4096;
  const MAX_FRAME = HEADER_SIZE + MAX_PAYLOAD + CRC_SIZE;
  const MODULE_CALIBRATION = 5;
  const MODULE_DISCOVERY = 0xFF, T_LIST_MODULES = 0x00;
  const FLAGS_VERSION = 1;
  const FLAGS_REQUEST = (FLAGS_VERSION << 4) | 0; // 0x10
  const FLAGS_REPLY_BIT = 0x01;
  const FLAG_CORRELATION = 0x02;
  const TYPE = {
    GET_STATE: 0x50,
    SET_MANUAL_OFFSET: 0x51,
    SET_DEADZONE_OVERRIDE: 0x52,
    SET_NUDGE_SETTINGS: 0x53,
    SET_INPUT_MODE: 0x54,
    SAVE_TO_FLASH: 0x55,
    START_CALIBRATION: 0x56,
    STATE: 0xD0,
    CALIBRATION_RESULT: 0xD1,
    OK: 0xDF,
    ERROR: 0xFE,
  };
  const INPUT_MODE = { MOUSE: 0, XAC: 1 };

  // CRC-32 (zlib / IEEE 802.3). Golden: crc32("123456789") === 0xCBF43926.
  const CRC_TABLE = (() => {
    const table = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let c = i;
      for (let k = 0; k < 8; k++) c = (c & 1) ? ((c >>> 1) ^ 0xEDB88320) : (c >>> 1);
      table[i] = c >>> 0;
    }
    return table;
  })();
  function crc32(bytes) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
    return (~c) >>> 0;
  }

  function buildFrame(module, type, payload) {
    payload = payload || new Uint8Array(0);
    if (payload.length > MAX_PAYLOAD) throw new Error("payload exceeds " + MAX_PAYLOAD + " bytes");
    const frame = new Uint8Array(HEADER_SIZE + payload.length + CRC_SIZE);
    const view = new DataView(frame.buffer);
    view.setUint16(0, 0x4F54, true);   // magic 'T''O' on the wire
    frame[2] = FLAGS_REQUEST;
    frame[3] = module;
    frame[4] = type;
    view.setUint32(5, payload.length, true);
    frame.set(payload, HEADER_SIZE);
    view.setUint32(HEADER_SIZE + payload.length,
                   crc32(frame.subarray(0, HEADER_SIZE + payload.length)), true);
    return frame;
  }

  class FrameParser {
    constructor() { this.buf = new Uint8Array(0); this.lastActivity = 0; }
    reset() { this.buf = new Uint8Array(0); }
    feed(chunk) {
      const merged = new Uint8Array(this.buf.length + chunk.length);
      merged.set(this.buf, 0); merged.set(chunk, this.buf.length);
      const frames = [];
      const textRanges = [];
      let textLen = 0;
      const skip = (start, end) => {
        if (end <= start) return;
        const last = textRanges[textRanges.length - 1];
        if (last && last[1] === start) last[1] = end;
        else textRanges.push([start, end]);
        textLen += end - start;
      };
      let pos = 0;
      for (;;) {
        const runStart = pos;
        while (pos < merged.length &&
               !(merged[pos] === 0x54 && (pos + 1 >= merged.length || merged[pos + 1] === 0x4F))) {
          pos++;
        }
        skip(runStart, pos);
        if (merged.length - pos < 3) break;
        const flags = merged[pos + 2];
        const ext = (flags & FLAG_CORRELATION) ? 2 : 0;
        const headerSize = HEADER_SIZE + ext;
        if (merged.length - pos < headerSize) break;
        const view = new DataView(merged.buffer, merged.byteOffset + pos);
        const len = view.getUint32(5 + ext, true);
        if (len > MAX_PAYLOAD) { skip(pos, pos + 1); pos++; continue; }
        const total = headerSize + len + CRC_SIZE;
        if (merged.length - pos < total) break;
        const expected = view.getUint32(headerSize + len, true);
        const actual = crc32(merged.subarray(pos, pos + headerSize + len));
        if (actual !== expected) { skip(pos, pos + 1); pos++; continue; }
        const correlation = ext ? (merged[pos + 5] | (merged[pos + 6] << 8)) : null;
        frames.push({ flags,
                      reply: (flags & FLAGS_REPLY_BIT) !== 0,
                      correlation,
                      module: merged[pos + 3],
                      type: merged[pos + 4],
                      payload: merged.slice(pos + headerSize, pos + headerSize + len) });
        pos += total;
      }
      this.buf = merged.slice(pos);
      this.lastActivity = Date.now();
      const text = new Uint8Array(textLen);
      let off = 0;
      for (const [start, end] of textRanges) {
        text.set(merged.subarray(start, end), off);
        off += end - start;
      }
      return { frames, text };
    }
    flushStale(maxAgeMs) {
      if (this.buf.length === 0 || Date.now() - this.lastActivity < maxAgeMs) return null;
      const text = this.buf;
      this.buf = new Uint8Array(0);
      return text;
    }
  }

  function readU32(payload, offset) {
    return new DataView(payload.buffer, payload.byteOffset + offset, 4).getUint32(0, true);
  }
  function parseErrorReply(payload) {
    if (payload.length < 4) return { code: 0, message: "(malformed error reply)" };
    let message = "";
    try { message = new TextDecoder().decode(payload.subarray(4)); } catch (_) {}
    return { code: readU32(payload, 0), message };
  }

  function parseDiscovery(payload) {
    let i = 0;
    const need = (n) => { if (i + n > payload.length) throw new Error("truncated discovery payload"); };
    const u8 = () => { need(1); return payload[i++]; };
    const str = () => { const n = u8(); need(n); const s = new TextDecoder().decode(payload.subarray(i, i + n)); i += n; return s; };
    const version = u8(); u8();
    const device = str(), fw = str();
    const count = u8();
    const modules = [];
    for (let m = 0; m < count; m++) modules.push({ id: u8(), name: str(), app: str(), desc: str() });
    return { version, device, fw, modules };
  }

  // Fixed 79-byte STATE struct; see calibration_service.hpp.
  function decodeState(payload) {
    if (payload.length < 79) throw new Error("malformed STATE reply (" + payload.length + " B, expected 79)");
    const v = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    let o = 0;
    const f32 = () => { const x = v.getFloat32(o, true); o += 4; return x; };
    const u8 = () => payload[o++];
    const u32 = () => { const x = v.getUint32(o, true); o += 4; return x; };
    const s = {
      centerX: f32(), centerY: f32(), centerNoise: f32(), centerDeadzoneAuto: f32(),
      manualOffsetX: f32(), manualOffsetY: f32(),
      rangeMinX: f32(), rangeMaxX: f32(), rangeMinY: f32(), rangeMaxY: f32(),
      usableRadius: f32(), reliableMaxRadius: f32(), normalizedMaxRadius: f32(), rangeDeadzoneAuto: f32(),
    };
    const hasCenterDz = u8() !== 0;
    const manualCenterDz = f32();
    const hasRangeDz = u8() !== 0;
    const manualRangeDz = f32();
    s.manualCenterDeadzone = hasCenterDz ? manualCenterDz : null;
    s.manualRangeDeadzone = hasRangeDz ? manualRangeDz : null;
    s.nudgeEnterThreshold = f32();
    s.nudgeExitThreshold = f32();
    s.nudgeTimeMs = u32();
    s.inputMode = u8();
    return s;
  }

  // Request payload encoders (GET_STATE, SAVE_TO_FLASH and START_CALIBRATION
  // carry no payload, so the console passes null for them).
  function encodeSetManualOffset(x, y) {
    const payload = new Uint8Array(8);
    const v = new DataView(payload.buffer);
    v.setFloat32(0, x, true);
    v.setFloat32(4, y, true);
    return payload;
  }
  function encodeSetDeadzoneOverride(centerOn, centerVal, rangeOn, rangeVal) {
    const payload = new Uint8Array(9);
    const v = new DataView(payload.buffer);
    payload[0] = (centerOn ? 0x01 : 0) | (rangeOn ? 0x02 : 0);
    v.setFloat32(1, centerOn ? centerVal : 0, true);
    v.setFloat32(5, rangeOn ? rangeVal : 0, true);
    return payload;
  }
  function encodeSetNudgeSettings(enter, exit, timeMs) {
    const payload = new Uint8Array(12);
    const v = new DataView(payload.buffer);
    v.setFloat32(0, enter, true);
    v.setFloat32(4, exit, true);
    v.setUint32(8, timeMs, true);
    return payload;
  }
  function encodeSetInputMode(mode) {
    return new Uint8Array([mode]);
  }

  const OmegaProtocol = {
    HEADER_SIZE, CRC_SIZE, MAX_PAYLOAD, MAX_FRAME,
    MODULE_CALIBRATION, MODULE_DISCOVERY, T_LIST_MODULES,
    FLAGS_REQUEST, FLAGS_REPLY_BIT, FLAG_CORRELATION,
    TYPE, INPUT_MODE,
    crc32, buildFrame, FrameParser, readU32,
    parseErrorReply, parseDiscovery, decodeState,
    encodeSetManualOffset, encodeSetDeadzoneOverride,
    encodeSetNudgeSettings, encodeSetInputMode,
  };
  globalThis.OmegaProtocol = OmegaProtocol;
  if (typeof module !== "undefined" && module.exports) module.exports = OmegaProtocol;
})();
