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
  const PROTOCOL_ID = "omega.calibration", PROTOCOL_VERSION = 1; // dispatcher discovery v2
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
    RESTART: 0x57,
    OTA_BEGIN: 0x58,
    OTA_DATA: 0x59,
    OTA_FINISH: 0x5A,
    OTA_ABORT: 0x5B,
    GET_FW_INFO: 0x5C,
    MARK_VALID: 0x5D,
    REVERT: 0x5E,
    GET_DIAGNOSTICS: 0x5F,
    STATE: 0xD0,
    CALIBRATION_RESULT: 0xD1,
    FW_INFO: 0xD2,
    DIAGNOSTICS: 0xD3,
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
    for (let m = 0; m < count; m++) {
      const rec = { id: u8(), name: str(), app: str(), desc: str() };
      // Discovery v2 records also carry [protocol str][protocol_version u16 LE].
      if (version >= 2) { rec.protocol = str(); need(2); rec.protocolVersion = payload[i] | (payload[i + 1] << 8); i += 2; }
      modules.push(rec);
    }
    return { version, device, fw, modules };
  }

  // Picks the dispatcher module id to talk to from a parseDiscovery() result.
  // Discovery v2 identifies the service by protocol id, so a service configured
  // on a non-default module id is still found. Discovery v1 has no protocol
  // field, so the default id is checked instead. When nothing matches, id stays
  // at the default (how === "missing").
  function selectCalibrationModule(info) {
    if (info.version >= 2) {
      const m = info.modules.find((x) => x.protocol === PROTOCOL_ID);
      if (m) {
        return {
          id: m.id, how: "protocol",
          versionMismatch: !!m.protocolVersion && m.protocolVersion !== PROTOCOL_VERSION,
        };
      }
    } else if (info.modules.some((x) => x.id === MODULE_CALIBRATION)) {
      return { id: MODULE_CALIBRATION, how: "legacy-id", versionMismatch: false };
    }
    return { id: MODULE_CALIBRATION, how: "missing", versionMismatch: false };
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

  // Variable-length DIAGNOSTICS reply; see calibration_service.hpp.
  function decodeDiagnostics(payload) {
    if (payload.length < 11) throw new Error("malformed DIAGNOSTICS reply (" + payload.length + " B, expected at least 11)");
    const v = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    let o = 0;
    const u8 = () => payload[o++];
    const u32 = () => { const x = v.getUint32(o, true); o += 4; return x; };
    const d = { sensorFault: u8(), sensorFirstFault: u8(), sensorFaultCount: u32(), bootCount: u32(), resets: [] };
    const count = u8();
    const utf8 = new TextDecoder();
    for (let i = 0; i < count; i++) {
      if (payload.length < o + 7) throw new Error("malformed DIAGNOSTICS reply: reset entry " + i + " truncated");
      const r = { boot: u32(), reason: u8(), abnormal: u8() !== 0 };
      const nameLen = u8();
      if (payload.length < o + nameLen) throw new Error("malformed DIAGNOSTICS reply: reset name " + i + " truncated");
      r.name = utf8.decode(payload.subarray(o, o + nameLen)); o += nameLen;
      d.resets.push(r);
    }
    return d;
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

  // ---------------------------------------------------------------------
  // Firmware image inspection and update planning (pure, no DOM).
  // Offsets come from esp_image_header_t (bootloader_support/esp_app_format.h)
  // and esp_app_desc_t (esp_app_format/esp_app_desc.h) in ESP-IDF.
  // ---------------------------------------------------------------------
  const OTA_SLOT_SIZE = 0x1E0000;       // ota_0 / ota_1 in partitions.csv (1920K)
  const CHIP_ID_ESP32S3 = 9;            // ESP_CHIP_ID_ESP32S3
  const DEFAULT_PROJECT = "omega_stick"; // project(omega_stick) in CMakeLists.txt
  const IMAGE_MAGIC = 0xE9;             // ESP_IMAGE_HEADER_MAGIC
  const APP_DESC_MAGIC = 0xABCD5432;    // ESP_APP_DESC_MAGIC_WORD
  const IMAGE_HEADER_SIZE = 24, SEGMENT_HEADER_SIZE = 8;
  const APP_DESC_OFFSET = IMAGE_HEADER_SIZE + SEGMENT_HEADER_SIZE; // 32
  const APP_DESC_SIZE = 256;
  const MAX_SEGMENTS = 16;              // ESP_IMAGE_MAX_SEGMENTS
  const CHIP_NAMES = {
    0: "ESP32", 2: "ESP32-S2", 5: "ESP32-C3", 9: "ESP32-S3", 12: "ESP32-C2", 13: "ESP32-C6",
    16: "ESP32-H2", 18: "ESP32-P4", 20: "ESP32-C61", 23: "ESP32-C5", 25: "ESP32-H21", 28: "ESP32-H4",
  };

  function cString(bytes, start, len) {
    let end = start;
    const limit = Math.min(bytes.length, start + len);
    while (end < limit && bytes[end] !== 0) end++;
    let s = "";
    for (let i = start; i < end; i++) s += String.fromCharCode(bytes[i]);
    return s;
  }
  function hexOf(bytes, start, len) {
    let s = "";
    for (let i = start; i < start + len; i++) s += bytes[i].toString(16).padStart(2, "0");
    return s;
  }
  function formatBytes(n) {
    return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ",") + " B";
  }

  // Inspects a firmware file before it is sent. Returns
  //   { ok, errors: [{code, message}], warnings: [{code, message}], info }
  // where ok is true when there are no errors. info is null until the file is
  // recognised as an ESP image, and has appDesc set only for an app image.
  // options: expectedProject (default "omega_stick"), slotSize, chipId.
  function inspectImage(bytes, options) {
    const opt = options || {};
    const slotSize = opt.slotSize || OTA_SLOT_SIZE;
    const chipId = opt.chipId === undefined ? CHIP_ID_ESP32S3 : opt.chipId;
    const project = opt.expectedProject || DEFAULT_PROJECT;
    const errors = [], warnings = [];
    const fail = (code, message) => { errors.push({ code, message }); };
    const result = (info) => ({ ok: errors.length === 0, errors, warnings, info: info || null });

    if (!bytes || bytes.length === 0) {
      fail("empty", "The file is empty.");
      return result();
    }
    if (bytes.length >= 4 && bytes[0] === 0x7F && bytes[1] === 0x45 && bytes[2] === 0x4C && bytes[3] === 0x46) {
      fail("elf", "This is an .elf file, not a firmware image. Choose the .bin file from the build folder instead.");
      return result();
    }
    if (bytes.length >= 2 && bytes[0] === 0xAA && bytes[1] === 0x50) {
      fail("partition_table", "This is a partition table, not a firmware image. Choose the main application .bin file (omega_stick.bin).");
      return result();
    }
    if (bytes[0] !== IMAGE_MAGIC) {
      fail("not_esp_image", "This is not an ESP32 firmware image. Choose the application .bin file from the build folder (omega_stick.bin).");
      return result();
    }
    if (bytes.length < IMAGE_HEADER_SIZE) {
      fail("truncated", "The file is cut short (" + bytes.length + " bytes). Copy or build it again.");
      return result();
    }
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const info = {
      size: bytes.length,
      segmentCount: bytes[1],
      chipId: v.getUint16(12, true),
      minChipRev: v.getUint16(15, true),
      maxChipRev: v.getUint16(17, true),
      hashAppended: bytes[23] === 1,
      appDesc: null,
    };
    info.chipName = CHIP_NAMES[info.chipId] || ("chip id " + info.chipId);

    if (info.segmentCount === 0 || info.segmentCount > MAX_SEGMENTS) {
      fail("not_esp_image", "This does not look like a valid ESP32 firmware image (bad header). Choose the application .bin file.");
      return result(info);
    }
    if (info.chipId !== chipId) {
      fail("wrong_chip", "This firmware is built for the " + info.chipName + ", but this device is an " +
           (CHIP_NAMES[chipId] || ("chip id " + chipId)) + ". Flashing it would not work.");
    }

    // Walk the segment list to learn how long a complete image must be.
    let pos = IMAGE_HEADER_SIZE, walkOk = true;
    for (let i = 0; i < info.segmentCount; i++) {
      if (pos + SEGMENT_HEADER_SIZE > bytes.length) { walkOk = false; break; }
      pos += SEGMENT_HEADER_SIZE + v.getUint32(pos + 4, true);
      if (pos > bytes.length) { walkOk = false; break; }
    }
    if (walkOk) {
      // The checksum byte ends the 16-byte block after the last segment, then comes the optional SHA-256.
      info.expectedSize = (pos | 15) + 1 + (info.hashAppended ? 32 : 0);
      if (bytes.length < info.expectedSize) walkOk = false;
    }
    if (!walkOk) {
      fail("truncated", "The file is incomplete (it ends before the image does). Build it again or copy it again.");
    }

    // App descriptor: first bytes of the first segment's data.
    const hasDesc = bytes.length >= APP_DESC_OFFSET + 4 && v.getUint32(APP_DESC_OFFSET, true) === APP_DESC_MAGIC;
    if (!hasDesc) {
      if (walkOk) {
        fail("not_app_image", "This is an ESP32 image but not an application (it could be a bootloader). Choose the application .bin file (omega_stick.bin).");
      }
    } else if (bytes.length < APP_DESC_OFFSET + APP_DESC_SIZE) {
      // The descriptor starts but the file ends inside it: there is no version
      // or project to read, so it must not pass as a usable app image.
      if (walkOk) {
        fail("truncated", "The file ends inside the firmware's description block, so it is incomplete. Build it again or copy it again.");
      }
    } else {
      const d = APP_DESC_OFFSET;
      info.appDesc = {
        secureVersion: v.getUint32(d + 4, true),
        version: cString(bytes, d + 16, 32),
        project: cString(bytes, d + 48, 32),
        time: cString(bytes, d + 80, 16),
        date: cString(bytes, d + 96, 16),
        idf: cString(bytes, d + 112, 32),
        sha256: hexOf(bytes, d + 144, 32),
      };
      info.appDesc.shaShort = info.appDesc.sha256.slice(0, 8);
      if (info.appDesc.project !== project) {
        fail("wrong_project", "This firmware is for a different product (\"" + (info.appDesc.project || "unnamed") +
             "\" instead of \"" + project + "\"). Choose the " + project + " application .bin file.");
      }
      if (info.appDesc.secureVersion !== 0) {
        warnings.push({ code: "secure_version", message: "This image sets a secure (anti-rollback) version of " + info.appDesc.secureVersion + "." });
      }
    }

    if (bytes.length > slotSize) {
      fail("too_big", "The file is " + formatBytes(bytes.length) + ", which is larger than the " + formatBytes(slotSize) +
           " update slot on the device. It cannot be installed.");
    }
    return result(info);
  }

  // Parses X.Y.Z, X.Y.Z+sha, X.Y.Z+sha.dirty (see D19). Anything else is invalid.
  function parseVersion(text) {
    const raw = String(text === undefined || text === null ? "" : text).trim();
    const m = /^v?(\d+)\.(\d+)\.(\d+)(?:\+([0-9A-Za-z]+))?(\.dirty)?$/.exec(raw);
    if (!m) return { valid: false, raw, dev: false };
    return {
      valid: true, raw, major: +m[1], minor: +m[2], patch: +m[3],
      sha: m[4] || "", dirty: !!m[5], dev: !!(m[4] || m[5]),
    };
  }

  // running: { version, sha } from FW_INFO (sha = 8 hex chars of the ELF hash).
  // candidate: { version, sha } (sha may be the full 64 hex or the short form).
  // kind: upgrade | downgrade | same-version | same-build | dev | unknown.
  function compareFirmware(running, candidate) {
    const r = parseVersion(running && running.version), c = parseVersion(candidate && candidate.version);
    const rs = String((running && running.sha) || "").toLowerCase();
    const cs = String((candidate && candidate.sha) || "").toLowerCase();
    const n = Math.min(rs.length, cs.length);
    const sameBuild = n >= 8 && rs.slice(0, n) === cs.slice(0, n);
    let kind;
    if (sameBuild) kind = "same-build";
    else if (!r.valid || !c.valid) kind = "unknown";
    else {
      const d = (c.major - r.major) || (c.minor - r.minor) || (c.patch - r.patch);
      if (d > 0) kind = "upgrade";
      else if (d < 0) kind = "downgrade";
      else kind = (r.dev || c.dev) ? "dev" : "same-version";
    }
    return { kind, sameBuild, runningDev: r.dev, candidateDev: c.dev };
  }

  const KIND_LABEL = {
    "upgrade": "Upgrade",
    "downgrade": "Downgrade (older than the running version)",
    "same-version": "Same version number, different build",
    "same-build": "Same build as the running firmware",
    "dev": "Development build (same base version)",
    "unknown": "Version could not be compared",
  };

  function kindWarnings(cmp, running) {
    const w = [];
    const rv = (running && running.version) || "unknown";
    if (cmp.kind === "downgrade") {
      w.push({ code: "downgrade", message: "This is an older version than the one running (" + rv +
               "). Going back is allowed, but the older firmware may lack recent fixes." });
    } else if (cmp.kind === "same-build") {
      w.push({ code: "same_build", message: "This exact build is already running. Flashing it again changes nothing." });
    } else if (cmp.kind === "same-version") {
      w.push({ code: "same_version", message: "This has the same version number as the running firmware, but it is a different build." });
    } else if (cmp.kind === "unknown") {
      w.push({ code: "version_unknown", message: "The versions could not be compared, so it is not clear whether this is newer." });
    }
    if (cmp.candidateDev && cmp.kind !== "same-build") {
      w.push({ code: "dev_build", message: "This is a development build (its version has a commit suffix), not a tagged release." });
    }
    return w;
  }

  // Everything the confirm panel needs for a Flash. inspection is the result of
  // inspectImage, running is FW_INFO ({ version, sha }) or null when it could
  // not be read, fileInfo is { name, size }.
  function buildFlashPlan(inspection, running, fileInfo) {
    const errors = inspection.errors.slice();
    const warnings = inspection.warnings.slice();
    const app = inspection.info && inspection.info.appDesc;
    const plan = {
      blocked: errors.length > 0, errors, warnings,
      fileName: (fileInfo && fileInfo.name) || "firmware.bin",
      size: (fileInfo && fileInfo.size) || (inspection.info ? inspection.info.size : 0),
      kind: "unknown", kindLabel: "", headline: "",
      newVersion: app ? app.version : "", newSha: app ? app.shaShort : "",
    };
    if (!app) return plan;
    const newLabel = app.version || "unknown";
    if (!running || !running.version) {
      warnings.push({ code: "running_unknown", message: "The running firmware version could not be read, so the new version cannot be compared with it." });
      plan.headline = "New " + newLabel;
      return plan;
    }
    const cmp = compareFirmware(running, { version: app.version, sha: app.shaShort });
    plan.kind = cmp.kind;
    plan.kindLabel = KIND_LABEL[cmp.kind];
    plan.headline = "Running " + running.version + " -> New " + newLabel;
    for (const w of kindWarnings(cmp, running)) warnings.push(w);
    return plan;
  }

  // Previous-slot states FW_INFO reports that can be reverted to. "empty" (no
  // image) and "partial" (an interrupted upload left an image that cannot be
  // verified) cannot, nor can an unknown state from older firmware.
  const REVERTIBLE_STATES = ["valid", "new", "pending_verify", "invalid", "aborted", "undefined"];
  const PARTIAL_SLOT_NOTE = "An interrupted upload left a partial image in the other slot, so there is no previous " +
    "firmware to go back to. Upload new firmware to replace it.";
  // info: parsed FW_INFO or null. Returns { usable, reason } for the Revert button.
  function revertAvailability(info) {
    if (!info) return { usable: false, reason: "The firmware information has not been read from the device." };
    const st = info.prevState || "";
    if (REVERTIBLE_STATES.includes(st)) return { usable: true, reason: "" };
    if (st === "partial") return { usable: false, reason: PARTIAL_SLOT_NOTE };
    return { usable: false, reason: "No previous firmware in the other slot" };
  }
  // The "Previous state" field: FW_INFO's state name, explained where it needs it.
  function describePrevState(state) {
    if (state === "partial") return "partial (an interrupted upload, cannot be used)";
    if (state === "empty") return "empty (no firmware)";
    return state || "n/a";
  }

  // prev: { version, label, state } from FW_INFO. Used for the Revert panel.
  function buildRevertPlan(running, prev) {
    const warnings = [], errors = [];
    const pv = (prev && prev.version) || "";
    if (prev && prev.state === "partial") errors.push({ code: "partial_previous", message: PARTIAL_SLOT_NOTE });
    else if (!pv) errors.push({ code: "no_previous", message: "The other slot has no firmware to go back to." });
    const plan = { blocked: errors.length > 0, errors, warnings, kind: "unknown", kindLabel: "", headline: "" };
    if (plan.blocked) return plan;
    const rv = (running && running.version) || "unknown";
    plan.headline = "Running " + rv + " -> Previous " + pv;
    const cmp = compareFirmware(running || {}, { version: pv, sha: "" });
    plan.kind = cmp.kind;
    plan.kindLabel = KIND_LABEL[cmp.kind];
    if (plan.kind === "upgrade") {
      warnings.push({ code: "revert_newer", message: "The previous slot holds a newer version than the one running." });
    }
    if (plan.kind === "same-version" || plan.kind === "dev") {
      warnings.push({ code: "same_version", message: "The previous firmware has the same base version number as the running one." });
    }
    const st = (prev && prev.state) || "";
    if (st && st !== "valid") {
      warnings.push({ code: "prev_state", message: "The previous firmware is in state \"" + st +
                      "\". After the restart it has to be confirmed again, or the device goes back." });
    }
    return plan;
  }

  // Compares the build the page expects after a reboot with what FW_INFO now
  // reports. expected: { kind: "flash" | "revert", version, sha? (short or
  // full), before: { version, sha }, uncertain? }. uncertain is set when the
  // OTA_FINISH reply was lost with the link, so it is not known whether the
  // image was installed. after: parsed FW_INFO ({ version, sha, pending,
  // running?, boot? }) or null when nothing could be read.
  // state: confirming | confirmed | rolled_back | not_restarted | unexpected | same_build | no_device.
  // not_restarted: the boot slot was switched but the device still runs the
  // old image (the reply and the restart were lost); a restart starts it.
  function decideOutcome(expected, after) {
    const before = expected.before || {};
    const revert = expected.kind === "revert";
    const noun = revert ? "Reverted to" : "Updated to";
    const notRestarted = () => ({
      state: "not_restarted",
      message: (revert ? "The previous firmware is" : "The new firmware is") +
               " installed, but the device has not restarted into it yet. Use Restart now: the device then starts it, " +
               "and this page checks and confirms it.",
    });
    if (!after || !after.version) {
      return { state: "no_device", message: "The device did not come back, or its firmware version could not be read." };
    }
    const sameAs = (a, b) => {
      if (!a || !b || !a.version || !b.version || a.version !== b.version) return false;
      const x = String(a.sha || "").toLowerCase(), y = String(b.sha || "").toLowerCase();
      const n = Math.min(x.length, y.length);
      return n < 8 || x.slice(0, n) === y.slice(0, n); // sha unknown on either side: version alone
    };
    const isNew = sameAs(after, expected);
    const isOld = sameAs(after, before);
    const switched = !after.pending && !!after.boot && !!after.running && after.boot !== after.running;
    if (isNew && isOld) {
      // Reflashing the identical build: only the pending flag tells.
      if (after.pending) {
        return { state: "confirming", message: noun + " " + after.version + ". Checking it works..." };
      }
      if (switched) return notRestarted();
      return { state: "same_build", message: "The device is running " + after.version +
               ", the same build as before, so it cannot be told whether the new copy started." };
    }
    if (isNew) {
      if (after.pending) return { state: "confirming", message: noun + " " + after.version + ". Checking it works..." };
      return { state: "confirmed", message: "Update confirmed. The device is running " + after.version + "." };
    }
    if (isOld) {
      if (switched) return notRestarted();
      if (revert) {
        return { state: "rolled_back",
                 message: "The device is running " + after.version + " again, not the previous firmware (" +
                          (expected.version || "unknown") + "). The previous firmware did not start, or it was not confirmed." };
      }
      if (expected.uncertain) {
        return { state: "rolled_back",
                 message: "The update did not complete, so the device is still running " + after.version + "." };
      }
      return { state: "rolled_back",
               message: "The new firmware was not confirmed, so the device went back to " + after.version + "." };
    }
    return { state: "unexpected",
             message: "The device is running " + after.version + ", which is not the version that was installed (" +
                      (expected.version || "unknown") + ")." };
  }

  // The checks the console runs after a reboot into a pending image, in order,
  // before it sends MARK_VALID. To add a check: add an entry here, a case in
  // judgeConfirmCheck, and one line gathering its input in the page.
  const CONFIRM_CHECKS = [
    { id: "firmware", label: "Checking the running firmware" },
    { id: "protocol", label: "Checking the connection" },
    { id: "sensor", label: "Checking the sensor" },
  ];
  // DIAGNOSTICS sensor_fault codes. 3 is not a fault: the firmware has not yet
  // seen the sensor answer since boot (D22), which is different from healthy.
  const SENSOR_NOT_OBSERVED = 3;
  const SENSOR_FAULT_LABEL = { 1: "read failed", 2: "reading stuck" };
  const SENSOR_STATE_NAMES = { 0: "none", 1: "read failed", 2: "reading stuck", 3: "waiting for first readings" };
  function describeSensorState(code) {
    return SENSOR_STATE_NAMES[code] || ("code " + code);
  }
  // How the sensor check waits: poll GET_DIAGNOSTICS this often, for up to this
  // long while the firmware still says "not observed"; then, once it says
  // healthy, ask again at least SENSOR_DWELL_MS later and require healthy again
  // (so firmware that cannot say "not observed" is still given time to fail).
  const SENSOR_POLL_MS = 500;
  const SENSOR_WAIT_MS = 10000;
  const SENSOR_DWELL_MS = 1500;

  // Judges one check from the input the page gathered. Returns
  // { ok, skipped, message }; message explains a failure or a skip.
  //   firmware: { expected, after }  as for decideOutcome; must be "confirming"
  //   protocol: { error }            error text of a failed GET_STATE, else ""
  //   sensor:   { diagnostics, error }  decoded DIAGNOSTICS (decodeDiagnostics)
  //             or the error text of a failed GET_DIAGNOSTICS. A device that
  //             replies function_not_supported has older firmware: skipped.
  //             Code 3 (not observed yet) gives { ok: false, wait: true }: not a
  //             failure, ask again (see awaitSensorHealthy).
  // A firmware check that finds the expected image already confirmed gives
  // { ok: true, alreadyConfirmed: true }: nothing is left to confirm.
  function judgeConfirmCheck(id, input) {
    input = input || {};
    if (id === "firmware") {
      const o = decideOutcome(input.expected || {}, input.after || null);
      if (o.state === "confirming") return { ok: true, skipped: false, message: "" };
      if (o.state === "confirmed") return { ok: true, skipped: false, alreadyConfirmed: true, message: o.message };
      return { ok: false, skipped: false, message: o.message };
    }
    if (id === "protocol") {
      return input.error
        ? { ok: false, skipped: false, message: "The device did not answer a status request (" + input.error + ")." }
        : { ok: true, skipped: false, message: "" };
    }
    if (id === "sensor") {
      if (input.error) {
        if (describeUpdateError(input.error).category === "unsupported") {
          return { ok: true, skipped: true,
                   message: "This firmware does not report sensor status, so the sensor check was skipped." };
        }
        return { ok: false, skipped: false, message: "The sensor status could not be read (" + input.error + ")." };
      }
      const code = input.diagnostics ? input.diagnostics.sensorFault : undefined;
      if (code === undefined) return { ok: false, skipped: false, message: "The sensor status could not be read." };
      if (code === SENSOR_NOT_OBSERVED) {
        return { ok: false, skipped: false, wait: true,
                 message: "The sensor has not produced its first readings yet." };
      }
      if (code !== 0) {
        return { ok: false, skipped: false,
                 message: "The sensor reported a fault (" + (SENSOR_FAULT_LABEL[code] || ("code " + code)) + ")." };
      }
      return { ok: true, skipped: false, message: "" };
    }
    return { ok: false, skipped: false, message: "Unknown check: " + id + "." };
  }

  // The sensor check over time. read() gathers one input for judgeConfirmCheck
  // ("sensor"); sleep(ms) and now() are injected so tests need no real time;
  // opts.abort() stops early; opts.onWait(message) reports a wait. Returns the
  // final judgement { ok, skipped, message }.
  //   not observed  -> ask again every SENSOR_POLL_MS up to SENSOR_WAIT_MS, then
  //                    fail ("not producing readings"): no MARK_VALID
  //   fault / error -> fail at once
  //   unsupported   -> skipped at once (older firmware)
  //   healthy       -> must still be healthy SENSOR_DWELL_MS later
  async function awaitSensorHealthy(read, sleep, now, opts) {
    opts = opts || {};
    const started = now();
    let healthySince = null;
    for (;;) {
      if (opts.abort && opts.abort()) return { ok: false, skipped: false, aborted: true, message: "" };
      const r = judgeConfirmCheck("sensor", await read());
      if (r.skipped || (!r.ok && !r.wait)) return r;
      if (r.ok) {
        if (healthySince === null) healthySince = now();
        else if (now() - healthySince >= SENSOR_DWELL_MS) return r;
      } else {
        healthySince = null;
        if (now() - started >= SENSOR_WAIT_MS) {
          return { ok: false, skipped: false,
                   message: "The sensor was still not producing readings after " + Math.round(SENSOR_WAIT_MS / 1000) + " seconds." };
        }
        if (opts.onWait) opts.onWait(r.message);
      }
      await sleep(SENSOR_POLL_MS);
    }
  }

  // Runs one confirm check: the sensor check waits (awaitSensorHealthy), the
  // others are judged once from what gather() returns.
  async function runConfirmCheck(id, gather, env) {
    env = env || {};
    if (id === "sensor") return awaitSensorHealthy(gather, env.sleep, env.now, env);
    return judgeConfirmCheck(id, await gather());
  }

  // ---- Update flow state (upd.phase in the page) ----
  // running: writing the image or sending REVERT. rebooting: waiting for the
  // device to restart and come back. confirming: the checks before MARK_VALID.
  // waiting_rollback: a check failed; the device goes back at its deadline.
  // switched_no_restart: the boot slot changed but the OTA_FINISH or REVERT
  // reply was lost, and with it the restart. link_lost: Disconnect was used
  // during one of the last three; the next connect picks the update up again.
  // done (or no update at all): nothing in flight.
  // The page keeps listening for the device this long after the link dropped.
  // The firmware goes back to the previous image 120 s after a pending boot,
  // and its boot flash window (OMEGA_STICK_BOOT_FLASH_WINDOW_S) can hold USB
  // back for up to 60 s, so 40 s was too short.
  const RECONNECT_WINDOW_MS = 110000;
  function updateActive(phase) { return !!phase && phase !== "done"; }
  function resumesOnReconnect(phase) { return phase === "rebooting" || phase === "link_lost"; }
  function reconnectDeadline(droppedAt) { return droppedAt + RECONNECT_WINDOW_MS; }
  // Status text while the page waits for the device after a restart.
  function reconnectWaitText(leftMs, elapsedMs) {
    const left = Math.max(0, Math.round(leftMs / 1000));
    let t = "Waiting for the device to come back (" + left + " s left).";
    if (elapsedMs >= 15000) t += " After a restart it can take up to a minute to appear again.";
    return t;
  }

  const LOCK_REASON = {
    running: "An update is in progress. These controls come back when it has finished.",
    waiting_rollback: "The new firmware was not confirmed, and the device is about to go back to the previous firmware. " +
      "Wait for that to finish, then try again.",
    switched_no_restart: "The new firmware is installed but the device has not restarted into it. Use Restart now first.",
  };
  const PENDING_FLASH_REASON = "The running firmware is waiting to be confirmed, and the device refuses a new update until it is. " +
    "Use \"Mark running image valid\" to keep it, or wait for the device to go back to the previous firmware.";

  // Which update controls are usable. s: { connected, busy, latestBusy, phase
  // (upd.phase or null), reconnecting, pending (FW_INFO pending_verify),
  // revert (revertAvailability) }. Returns { connectLocked, modeLocked, flash,
  // install, revert, restart, markValid }; each control is { disabled, reason }
  // with reason "" when there is nothing to explain. While an update is in
  // flight anything that restarts the device or starts another update stays
  // locked, since any of them would force the pending image to roll back.
  function updateControls(s) {
    const phase = updateActive(s.phase) ? s.phase : "done";
    const active = phase !== "done";
    const conn = !!s.connected, busy = !!s.busy;
    const on = () => ({ disabled: false, reason: "" });
    const off = (reason) => ({ disabled: true, reason: reason || "" });
    const lock = active ? off(LOCK_REASON[phase] || LOCK_REASON.running) : null;
    let flash;
    if (!conn) flash = off("");
    else if (lock) flash = lock;
    else if (s.pending) flash = off(PENDING_FLASH_REASON);
    else flash = busy ? off("") : on();
    const install = flash.disabled ? flash : (s.latestBusy ? off("") : on());
    const rv = s.revert || { usable: false, reason: "" };
    let revert;
    if (conn && lock) revert = lock;
    else if (!rv.usable) revert = off(rv.reason);
    else revert = (!conn || busy) ? off("") : on();
    const restart = (!conn || busy) ? off("") : (lock && phase !== "switched_no_restart") ? lock : on();
    const markValid = (!conn || busy) ? off("") : (lock && phase !== "waiting_rollback") ? lock : on();
    const connectLocked = ["running", "confirming", "waiting_rollback", "switched_no_restart"].includes(phase) ||
      (phase === "rebooting" && (conn || !!s.reconnecting));
    return { connectLocked, modeLocked: active, flash, install, revert, restart, markValid };
  }

  // What happens to an update when the link goes away while the device is up
  // but the update is not settled. manual: the page's Disconnect was used (it is
  // locked during an update, so this is a fallback). Returns null when the
  // phase is not affected, else { phase, reconnect (start the automatic
  // reconnect), message }.
  function afterLinkLoss(phase, manual) {
    if (phase === "confirming") {
      return manual
        ? { phase: "link_lost", reconnect: false,
            message: "Disconnected before the new firmware was confirmed. The device goes back to the previous firmware by " +
                     "itself unless this page confirms it within about two minutes of its restart. Reconnect so this page can finish its checks." }
        : { phase: "rebooting", reconnect: true,
            message: "The connection dropped while the new firmware was being checked. Reconnecting to find out which firmware the device is running..." };
    }
    if (phase === "waiting_rollback") {
      return manual
        ? { phase: "link_lost", reconnect: false,
            message: "Disconnected. The new firmware was not confirmed, so the device goes back to the previous firmware by " +
                     "itself within about two minutes of its restart. Reconnect to see which firmware it runs." }
        : { phase: "rebooting", reconnect: true,
            message: "The connection dropped; the device is probably going back to the previous firmware. Reconnecting to find out which firmware it is running..." };
    }
    if (phase === "switched_no_restart") {
      return manual
        ? { phase: "link_lost", reconnect: false,
            message: "Disconnected. The new firmware is installed but has not started yet. Reconnect, then use Restart now so " +
                     "this page can check and confirm it. If the device restarts without this page, it goes back to the current firmware about two minutes later." }
        : { phase: "rebooting", reconnect: true,
            message: "The connection dropped. Reconnecting to find out which firmware the device is running..." };
    }
    return null;
  }

  // OTA_FINISH or REVERT got no reply in time. The firmware switches the boot
  // slot before it replies and only the delayed restart goes with the lost
  // reply, so the page reads FW_INFO again. kind: flash | revert. info: parsed
  // FW_INFO ({ running, boot }) or null. state: switched | not_switched | unknown.
  function decideAfterFinishTimeout(kind, info) {
    const what = kind === "revert" ? "the previous firmware" : "the new firmware";
    if (!info || !info.running || !info.boot) {
      return { state: "unknown",
               message: "The device stopped answering, and it is not known whether " + what + " was installed. " +
                        "Reconnect and check the Boot partition field before trying again." };
    }
    if (info.boot !== info.running) {
      return { state: "switched",
               message: (kind === "revert" ? "The previous firmware is" : "The new firmware is") +
                        " installed, but the device did not restart into it. Use Restart now: the device then starts it, " +
                        "and this page checks and confirms it." };
    }
    return { state: "not_switched",
             message: kind === "revert"
               ? "The device stopped answering and is still set to start the running firmware, so nothing was changed. Try again."
               : "The device stopped answering while it checked the new firmware, and it is still set to start the running firmware, so nothing was installed. Try again." };
  }

  // Whether closing or reloading the page now would leave an update half done.
  // s: { phase, uploading, downloading }.
  function shouldWarnBeforeUnload(s) {
    if (s.uploading || s.downloading) return true;
    return ["running", "rebooting", "confirming", "switched_no_restart", "link_lost"].includes(s.phase);
  }

  // Wording shown when a check failed and MARK_VALID was not sent.
  function confirmFailureNotice(message) {
    return message + " The new firmware was not confirmed. The device goes back to the previous firmware by " +
           "itself within about two minutes. To keep it anyway, use \"Mark running image valid\", which skips these checks.";
  }

  // Where a board is installed (or reinstalled) when it does not take updates
  // over USB. Also linked from the "unreachable board" hint in the console.
  const FIRST_FLASH_URL = "https://atdev-inc.github.io/omega-stick/flash.html";

  // Builds without updates over USB (CONFIG_OMEGA_STICK_OTA off, the v0.1.0
  // default) answer GET_FW_INFO with function_not_supported. Only that answer
  // means "this device has no USB updates"; a timeout, a dropped link or any
  // other failure is transient and must leave the update controls alone.
  // `error` is whatever GET_FW_INFO failed with, or null/undefined on success.
  function usbUpdatesUnavailable(error) {
    if (error === null || error === undefined) return false;
    return describeUpdateError(error).category === "unsupported";
  }

  // Outcome of an update whose new image turned out to have no USB updates
  // (GET_FW_INFO now replies function_not_supported). Such an image accepts
  // itself at boot, so there is nothing to confirm and no rollback to warn of.
  // `fw` is the firmware string from the discovery reply, or "" if unknown.
  function decideOutcomeNoUpdates(fw) {
    const running = fw ? " (" + fw + ")" : "";
    return { state: "installed_no_updates",
             message: "The new firmware is installed and running" + running +
                      ". It does not take further updates over USB, so new versions are installed from the first-flash page." };
  }

  // The plain-language note shown in place of the update controls. The
  // first-flash installer erases calibration and settings by default
  // (new_install_prompt_erase in the manifest), so the note says so.
  const NO_USB_UPDATES_NOTE =
    "This firmware does not take updates over USB. New versions are installed from the first-flash page. " +
    "Installing from there with its erase option ticked removes the saved calibration and settings, " +
    "so the stick needs calibrating again afterwards.";

  // Maps a raw failure (Error text from the page's transactions, device ERROR
  // replies, espp OTA error strings) to plain language. Returns
  // { category, title, advice }. Device ERROR replies read
  // "device error (code N): <context>: <errc text>"; the text is used in
  // preference to the number because std::errc numbers are not portable.
  function describeUpdateError(raw) {
    const t = String(raw === undefined || raw === null ? "" : (raw.message || raw));
    const has = (re) => re.test(t);
    const code = (/\(code (\d+)\)/.exec(t) || [])[1];
    const out = (category, title, advice) => ({ category, title, advice });
    if (has(/^not connected/i)) {
      return out("disconnected", "The device is not connected.", "Connect the device and try again.");
    }
    if (has(/transport lost|disconnected|upload cancelled|NetworkError|device has been lost/i)) {
      return out("lost", "The connection to the device was lost.",
        "Unplug the device, plug it in again, reconnect, and check which firmware it runs before trying again. A half-written update never replaces the working firmware.");
    }
    // Firmware that refuses a new upload while the running image is pending
    // says why; show its words rather than the generic "would not start".
    const refusal = /OTA_BEGIN refused: (the running image is not confirmed yet[^:]*)/i.exec(t);
    if (refusal) {
      const why = refusal[1].trim().replace(/[.;,]+$/, "");
      return out("not_confirmed", "The device refused the update: " + why + ".",
        "Confirm the running firmware with \"Mark running image valid\", or wait for the device to go back to the previous firmware, then try again.");
    }
    if (has(/timed out/i)) {
      return out("timeout", /OTA_BEGIN/.test(t) ? "The device took too long to get ready for the update."
                                                 : "The device stopped answering during the update.",
        "Wait a few seconds, reconnect, and try again. If it keeps happening, use a different USB cable or port.");
    }
    if (has(/Function not implemented|not wired on this build|function_not_supported/i) || code === "88") {
      return out("unsupported", "This device's firmware cannot be updated from here.",
        "Update it over the serial cable with the flashing tool instead.");
    }
    if (has(/other slot holds no app image|No such device/i)) {
      return out("no_previous", "There is no previous firmware to go back to.",
        "Nothing was changed. Flash a firmware file instead.");
    }
    if (has(/already in progress|upload is in progress|operation in progress/i)) {
      return out("busy", "Another update is already running on the device.",
        "Wait a minute or unplug and reconnect the device, then try again. Close other copies of this page first.");
    }
    if (has(/Device or resource busy|busy/i)) {
      return out("busy", "The device is busy and refused the update.",
        "Close other tabs or programs connected to the device (including the other connection type), wait about 30 seconds, and try again.");
    }
    if (has(/offset mismatch|no OTA session/i) || (/Operation not permitted/i.test(t) && /OTA/.test(t))) {
      return out("session", "The update session was lost part way.",
        "Nothing was installed. Reconnect and start the update again.");
    }
    if (has(/OTA_FINISH/)) {
      return out("rejected", "The device checked the new firmware and rejected it.",
        "The file may be damaged or built for something else. Nothing was installed. Build or copy the file again and retry.");
    }
    if (has(/Bad message|checksum|digest|invalid image|Invalid argument/i) && /OTA/.test(t)) {
      return out("rejected", "The device rejected the firmware data.",
        "The file may be damaged. Nothing was installed. Build or copy the file again and retry.");
    }
    if (has(/No space left|Not enough space|Cannot allocate memory/i)) {
      return out("too_big", "The firmware does not fit in the device's update slot.",
        "Check you chose the application .bin file for this device.");
    }
    if (has(/OTA_DATA|Input\/output error|write failed|stalled|USB write/i) || code === "5") {
      return out("write", "Writing to the device failed.",
        "Nothing was installed and the working firmware is untouched. Reconnect and try again, using a different USB port or cable if it repeats.");
    }
    if (has(/OTA_BEGIN/)) {
      return out("begin", "The device would not start the update.",
        "Reconnect the device and try again.");
    }
    if (has(/REVERT/)) {
      return out("revert", "The device could not switch back to the previous firmware.",
        "Nothing was changed. Reconnect and try again.");
    }
    return out("other", "The update did not complete.",
      "Reconnect the device and try again. The log below has the technical detail.");
  }

  // Progress text for the write phase.
  function formatProgress(written, total) {
    const t = total > 0 ? total : 1;
    const w = Math.max(0, Math.min(written, t));
    const percent = Math.floor((w * 100) / t);
    return { percent, text: "Writing " + percent + "% (" + formatBytes(w) + " of " + formatBytes(t) + ")" };
  }

  // ---------------------------------------------------------------------
  // Latest-release feed (published by CI, see scripts/release_feed.mjs).
  // ---------------------------------------------------------------------
  const FEED_URL = "https://atdev-inc.github.io/omega-stick/firmware/latest.json";
  const PAGES_ORIGIN = "https://atdev-inc.github.io";
  const PAGES_BASE = "/omega-stick/";
  const RELEASES_URL = "https://github.com/ATDev-Inc/omega-stick/releases";
  const NOTES_PREFIX = RELEASES_URL + "/";

  // Why a fetch() of the feed or the image threw. Browsers report a request
  // blocked by CORS and a network failure the same way, so "offline" is used
  // only when the browser says it has no connection. online: the onLine flag.
  // kind: timeout | offline | unreachable.
  function classifyFetchError(error, online) {
    if (error && error.name === "AbortError") return "timeout";
    if (online === false) return "offline";
    return "unreachable";
  }
  // Plain wording for a failed feed check. kind: classifyFetchError's kinds,
  // or missing | http | invalid.
  function describeFeedFailure(kind, status) {
    switch (kind) {
      case "offline": return "This computer is offline, so the release server could not be reached. Check your internet connection and try again.";
      case "timeout": return "The release server did not answer in time. Try again later.";
      case "unreachable": return "The release server could not be reached. The network, a firewall or proxy, or the browser's security " +
        "settings may have blocked it. Try again later, or look at the releases on GitHub.";
      case "missing": return "No release has been published yet.";
      case "http": return "The release server returned an error (" + status + "). Try again later.";
      default: return "The release information could not be read, so nothing was changed.";
    }
  }
  // Plain wording for a failed firmware download. kind: classifyFetchError's
  // kinds, or http | size | sha | nohash.
  function describeDownloadFailure(kind, status) {
    switch (kind) {
      case "offline": return "This computer is offline, so the firmware could not be downloaded. Nothing was changed.";
      case "timeout": return "The download stopped making progress, so it was cancelled. Nothing was changed. Try again.";
      case "unreachable": return "The firmware file could not be reached. The network, a firewall or proxy, or the browser's " +
        "security settings may have blocked it. Nothing was changed.";
      case "http": return "The release server could not provide the file (error " + status + "). Try again later.";
      case "size": return "The downloaded file is not the size the release says it should be, so it was not installed. Try again.";
      case "sha": return "The downloaded file did not match its checksum, so it was not installed. Try again, and if it keeps " +
        "happening, download the release from GitHub instead.";
      case "nohash": return "This browser cannot check the download, so it was not installed. Download the release from GitHub and choose the file instead.";
      default: return "The download failed, so nothing was changed.";
    }
  }

  // Where to read the feed from. Served from the project's own Pages site the
  // feed is a sibling path, so it stays same-origin; anywhere else (file://,
  // a local server, a fork) the published feed is used. There is deliberately
  // no way to override this from the page address.
  function feedUrlFor(locationHref) {
    try {
      const u = new URL(String(locationHref));
      if (u.origin === PAGES_ORIGIN && u.pathname.indexOf(PAGES_BASE) === 0) {
        return new URL("../firmware/latest.json", u.origin + u.pathname).href;
      }
    } catch (_) { /* fall through */ }
    return FEED_URL;
  }

  // Validates the parsed latest.json. Returns { ok, error, release } where
  // release is { version, tag, publishedAt, notesUrl, appUrl, size, sha256 }.
  function parseReleaseFeed(json, feedUrl) {
    const bad = (error) => ({ ok: false, error, release: null });
    if (!json || typeof json !== "object" || Array.isArray(json)) return bad("The release information is not in the expected format.");
    if (json.schema !== 1) return bad("The release information uses a format this page does not understand (schema " + json.schema + ").");
    if (json.project !== DEFAULT_PROJECT) return bad("The release information is for a different project.");
    if (json.chip !== "esp32s3") return bad("The release information is for a different chip.");
    if (typeof json.version !== "string" || !/^\d+\.\d+\.\d+$/.test(json.version)) return bad("The release version is not valid.");
    if (json.tag !== "v" + json.version) return bad("The release tag does not match its version.");
    const app = json.app;
    if (!app || typeof app !== "object") return bad("The release information has no firmware file.");
    if (!Number.isInteger(app.size) || app.size < 1 || app.size > OTA_SLOT_SIZE) return bad("The release firmware size is not valid.");
    if (typeof app.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(app.sha256)) return bad("The release firmware checksum is not valid.");
    if (typeof app.path !== "string" || !/^[A-Za-z0-9._-]+$/.test(app.path) || app.path.indexOf("..") >= 0) {
      return bad("The release firmware file name is not valid.");
    }
    let appUrl;
    try { appUrl = new URL(app.path, feedUrl).href; } catch (_) { return bad("The release address is not valid."); }
    const notesUrl = (typeof json.notes_url === "string" && json.notes_url.indexOf(NOTES_PREFIX) === 0) ? json.notes_url : "";
    const publishedAt = (typeof json.published_at === "string" && !Number.isNaN(Date.parse(json.published_at))) ? json.published_at : "";
    return {
      ok: true, error: "",
      release: { version: json.version, tag: json.tag, publishedAt, notesUrl, appUrl, fileName: app.path, size: app.size, sha256: app.sha256 },
    };
  }

  // Compares the running firmware ({ version, sha } from FW_INFO, or null)
  // with a parsed release. Returns { state, message, canInstall }.
  // state: update-available | up-to-date | running-newer | running-dev-build | unknown
  function summarizeAvailability(running, release) {
    const rv = running && running.version ? String(running.version) : "";
    const latest = release.version;
    if (!rv || !parseVersion(rv).valid) {
      return { state: "unknown", canInstall: false,
               message: "The latest release is " + latest + ". The version on your device could not be read, so it cannot be compared." };
    }
    const cmp = compareFirmware(running, { version: latest, sha: "" });
    switch (cmp.kind) {
      case "upgrade":
        return { state: "update-available", canInstall: true, message: "Firmware " + latest + " is available. You have " + rv + "." };
      case "same-version":
        return { state: "up-to-date", canInstall: false, message: "You have the latest release (" + latest + ")." };
      case "downgrade":
        return { state: "running-newer", canInstall: false,
                 message: "You have " + rv + ", which is newer than the latest release (" + latest + ")." };
      case "dev":
        return { state: "running-dev-build", canInstall: false,
                 message: "You are running a development build (" + rv + "). The latest release is " + latest + "." };
      default:
        return { state: "unknown", canInstall: false,
                 message: "The latest release is " + latest + ". Your version (" + rv + ") could not be compared." };
    }
  }

  // Chooses which already-granted port or device to reopen after a reboot.
  // link: { kind: "serial" | "usb", vid, pid, serial? } of the stick that was
  // connected. candidates: plain descriptors { ref, kind, opened, vid, pid,
  // serial?, isSaved } where isSaved is true for the exact port or device
  // object that was connected before. Never guesses between several sticks.
  // reason: exact | only-match | ambiguous | none.
  function pickReconnectCandidate(link, candidates) {
    const free = (candidates || []).filter((c) => c && !c.opened && c.kind === link.kind);
    const saved = free.find((c) => c.isSaved);
    if (saved) return { candidate: saved, reason: "exact" };
    let same = free.filter((c) => c.vid === link.vid && c.pid === link.pid);
    if (link.kind === "usb" && link.serial) {
      const bySerial = same.filter((c) => c.serial === link.serial);
      if (bySerial.length === 1) return { candidate: bySerial[0], reason: "exact" };
      same = bySerial;
    }
    if (same.length === 1) return { candidate: same[0], reason: "only-match" };
    return { candidate: null, reason: same.length > 1 ? "ambiguous" : "none" };
  }

  // ---- Simulator (omega_sim) WebSocket messages, see docs/simulator.md ----
  // Pure helpers: build and check the JSON control messages, parse telemetry,
  // validate uploaded scripts, and accumulate HID output for the output view.
  const SIM_PATTERNS = ["rest", "circle", "calibrate", "nudge_right", "nudge_left", "nudge_up", "nudge_down", "wander"];
  const SIM_FAULTS = ["ok", "no_response", "stuck_counter", "weak_magnet"];
  const SIM_NEXT_IMAGE = ["ok", "fail_self_test", "crash_loop", "slow_sensor_start", "slow_sensor_fail",
    "crash_when_confirmed"];
  const SIM_SOURCE_KINDS = ["manual", "pattern", "script"];
  const SIM_REPLIES = ["ok", "busy", "invalid", "unsupported", "error"];
  const SIM_ACTIONS = ["recalibrate", "save", "restart", "set_mode", "set_deadzone", "flash", "revert", "mark_valid", "fw_info"];
  const SIM_SCRIPT_MAX_BYTES = 262144, SIM_SCRIPT_MAX_STEPS = 5000;
  const SIM_MAX_AT_MS = 86400000; // a day
  const SIM_STEP_KEYS = ["stick", "pattern", "fault", "drift", "noise", "next_image", "action", "expect"];
  const SIM_EXPECT_KEYS = ["calibrated", "center_max", "mouse_moved", "clicked", "mode", "sensor_fault", "sensor_observed", "neutral", "fw_slot", "fw_pending", "fw_version", "xac_x", "xac_y"];

  const isNum = (v) => typeof v === "number" && Number.isFinite(v);
  const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
  const clampUnit = (v) => Math.max(-1, Math.min(1, v));

  function simNum(v, what) {
    if (!isNum(v)) throw new Error(what + " must be a finite number");
    return v;
  }
  function simOneOf(v, list, what) {
    if (typeof v !== "string" || !list.includes(v)) throw new Error(what + " must be one of " + list.join(", "));
    return v;
  }

  // Builds the JSON text for a page to simulator message. Throws an Error with
  // a plain message when the shape is wrong or a number is NaN or infinite.
  // Stick values are clamped to -1..1.
  function encodeSimMessage(msg) {
    if (!isObj(msg) || typeof msg.type !== "string") throw new Error("message needs a type");
    let out;
    switch (msg.type) {
      case "hello":
        out = { type: "hello", client: "console", version: 1 };
        break;
      case "stick":
        out = { type: "stick", x: clampUnit(simNum(msg.x, "x")), y: clampUnit(simNum(msg.y, "y")) };
        break;
      case "pattern":
        out = { type: "pattern", name: simOneOf(msg.name, SIM_PATTERNS, "pattern name") };
        break;
      case "script": {
        const v = validateSimScript(msg.script);
        if (!v.ok) throw new Error("script is not valid: " + v.errors[0]);
        out = { type: "script", script: msg.script };
        break;
      }
      case "stop": case "reset": case "wipe":
        out = { type: msg.type };
        break;
      case "fault":
        out = { type: "fault", sensor: simOneOf(msg.sensor, SIM_FAULTS, "sensor fault") };
        break;
      case "drift": {
        const rate = simNum(msg.rate, "drift rate");
        if (rate < 0 || rate > 10) throw new Error("drift rate must be 0 to 10");
        out = { type: "drift", rate };
        break;
      }
      case "noise": {
        const sigma = simNum(msg.sigma, "noise");
        if (sigma < 0 || sigma > 1) throw new Error("noise must be 0 to 1");
        out = { type: "noise", sigma };
        break;
      }
      case "next_image":
        out = { type: "next_image", behavior: simOneOf(msg.behavior, SIM_NEXT_IMAGE, "next image behavior") };
        break;
      default:
        throw new Error("unknown message type: " + msg.type);
    }
    return JSON.stringify(out);
  }

  // Parses a simulator to page text message. Returns a normalized object, or
  // null for anything that is not valid JSON, has an unknown type (ignored on
  // purpose so either side can add messages) or has a malformed body.
  function parseSimMessage(text) {
    let m;
    try { m = JSON.parse(text); } catch (_) { return null; }
    if (!isObj(m) || typeof m.type !== "string") return null;
    const str = (v) => (typeof v === "string" ? v : "");
    switch (m.type) {
      case "hello":
        return { type: "hello", sim: str(m.sim), version: isNum(m.version) ? m.version : 0,
                 fw: str(m.fw), mode: str(m.mode), slot: str(m.slot) };
      case "hid":
        if (m.mode === "mouse") {
          if (!isNum(m.dx) || !isNum(m.dy)) return null;
          return { type: "hid", mode: "mouse", dx: m.dx, dy: m.dy,
                   wheel: isNum(m.wheel) ? m.wheel : 0, buttons: isNum(m.buttons) ? (m.buttons | 0) : 0 };
        }
        if (m.mode === "xac") {
          if (!isNum(m.x) || !isNum(m.y)) return null;
          return { type: "hid", mode: "xac", x: clampUnit(m.x), y: clampUnit(m.y) };
        }
        return null;
      case "sensor":
        if (!isNum(m.x) || !isNum(m.y)) return null;
        return { type: "sensor", x: m.x, y: m.y, bx: isNum(m.bx) ? m.bx : 0, by: isNum(m.by) ? m.by : 0,
                 ok: m.ok === true, fault: str(m.fault) || "ok" };
      case "source":
        if (!SIM_SOURCE_KINDS.includes(m.kind)) return null;
        return { type: "source", kind: m.kind, name: str(m.name), t_ms: isNum(m.t_ms) ? m.t_ms : 0, done: m.done === true };
      case "rebooting":
        return { type: "rebooting", reason: str(m.reason) };
      case "error":
        return { type: "error", message: str(m.message) };
      default:
        return null;
    }
  }

  // Checks a script against the format in docs/simulator.md. Returns
  // { ok, errors } where errors are plain sentences (at most 20).
  function validateSimScript(script) {
    const errors = [];
    const add = (s) => { if (errors.length < 20) errors.push(s); };
    if (!isObj(script)) return { ok: false, errors: ["The script must be a JSON object."] };
    let size = 0;
    try { const text = JSON.stringify(script);
      size = typeof TextEncoder === "function" ? new TextEncoder().encode(text).length : text.length; } catch (_) { return { ok: false, errors: ["The script cannot be read as JSON."] }; }
    if (size > SIM_SCRIPT_MAX_BYTES) return { ok: false, errors: ["The script is larger than " + (SIM_SCRIPT_MAX_BYTES / 1024) + " KB."] };
    if (script.version !== 1) add("The script needs \"version\": 1.");
    if (script.name !== undefined && (typeof script.name !== "string" || script.name.length > 200)) {
      add("\"name\" must be text of at most 200 characters.");
    }
    for (const k of Object.keys(script)) {
      if (k !== "version" && k !== "name" && k !== "steps") add("Unknown script key \"" + k + "\".");
    }
    if (!Array.isArray(script.steps)) {
      add("The script needs a \"steps\" list.");
      return { ok: false, errors };
    }
    if (script.steps.length > SIM_SCRIPT_MAX_STEPS) {
      add("The script has " + script.steps.length + " steps; the limit is " + SIM_SCRIPT_MAX_STEPS + ".");
      return { ok: false, errors };
    }
    let prev = -Infinity;
    script.steps.forEach((step, i) => {
      const at = "Step " + (i + 1) + ": ";
      if (!isObj(step)) { add(at + "must be an object."); return; }
      if (!isNum(step.at_ms) || step.at_ms < 0 || step.at_ms > SIM_MAX_AT_MS) {
        add(at + "\"at_ms\" must be a number from 0 to " + SIM_MAX_AT_MS + ".");
      } else {
        if (step.at_ms < prev) add(at + "\"at_ms\" " + step.at_ms + " is earlier than the step before it; steps must be in time order.");
        prev = step.at_ms;
      }
      const present = SIM_STEP_KEYS.filter((k) => step[k] !== undefined);
      if (present.length !== 1) {
        add(at + (present.length === 0
          ? "needs one of " + SIM_STEP_KEYS.join(", ") + "."
          : "has more than one action (" + present.join(", ") + "); use exactly one."));
        return;
      }
      const key = present[0];
      const val = step[key];
      const allowed = ["at_ms", key];
      if (key === "pattern") allowed.push("duration_ms");
      if (key === "action") {
        allowed.push("value", "expect_reply");
        if (val === "flash") allowed.push("version");
      }
      for (const k of Object.keys(step)) if (!allowed.includes(k)) add(at + "unexpected key \"" + k + "\".");
      switch (key) {
        case "stick":
          if (!Array.isArray(val) || val.length !== 2 || !val.every((n) => isNum(n) && n >= -1 && n <= 1)) {
            add(at + "\"stick\" must be [x, y] with each value from -1 to 1.");
          }
          break;
        case "pattern":
          if (typeof val !== "string" || !SIM_PATTERNS.includes(val)) add(at + "unknown pattern. Known: " + SIM_PATTERNS.join(", ") + ".");
          if (step.duration_ms !== undefined && (!isNum(step.duration_ms) || step.duration_ms < 0 || step.duration_ms > SIM_MAX_AT_MS)) {
            add(at + "\"duration_ms\" must be a number from 0 to " + SIM_MAX_AT_MS + ".");
          }
          break;
        case "fault":
          if (typeof val !== "string" || !SIM_FAULTS.includes(val)) add(at + "unknown fault. Known: " + SIM_FAULTS.join(", ") + ".");
          break;
        case "drift":
          if (!isNum(val) || val < 0 || val > 10) add(at + "\"drift\" must be a number from 0 to 10.");
          break;
        case "noise":
          if (!isNum(val) || val < 0 || val > 1) add(at + "\"noise\" must be a number from 0 to 1.");
          break;
        case "next_image":
          if (typeof val !== "string" || !SIM_NEXT_IMAGE.includes(val)) add(at + "unknown next_image. Known: " + SIM_NEXT_IMAGE.join(", ") + ".");
          break;
        case "action":
          validateSimAction(step, at, add);
          break;
        case "expect":
          validateSimExpect(val, at, add);
          break;
      }
    });
    return { ok: errors.length === 0, errors };
  }

  function validateSimAction(step, at, add) {
    const a = step.action;
    if (typeof a !== "string" || !SIM_ACTIONS.includes(a)) {
      add(at + "unknown action. Known: " + SIM_ACTIONS.join(", ") + ".");
      return;
    }
    if (step.expect_reply !== undefined && !SIM_REPLIES.includes(step.expect_reply)) {
      add(at + "\"expect_reply\" must be one of " + SIM_REPLIES.join(", ") + ".");
    }
    if (a === "set_mode") {
      if (step.value !== "mouse" && step.value !== "xac") add(at + "set_mode needs \"value\": \"mouse\" or \"xac\".");
    } else if (a === "set_deadzone") {
      const v = step.value;
      if (!isObj(v)) { add(at + "set_deadzone needs a \"value\" object with \"center\" and/or \"range\"."); return; }
      const keys = Object.keys(v);
      if (keys.length === 0) add(at + "set_deadzone \"value\" needs \"center\" and/or \"range\".");
      for (const k of keys) {
        if (k !== "center" && k !== "range") add(at + "set_deadzone \"value\" has unknown key \"" + k + "\".");
      }
      // Out-of-range values are allowed only when the step expects the firmware
      // to refuse them (expect_reply "invalid"), see docs/simulator.md.
      const onPurpose = step.expect_reply === "invalid";
      const dz = (k, hi) => {
        if (v[k] === undefined) return;
        if (!isNum(v[k])) add(at + "deadzone " + k + " must be a number.");
        else if (!onPurpose && (v[k] < 0 || v[k] > hi)) add(at + "deadzone " + k + " must be 0 to " + hi + " (or use \"expect_reply\": \"invalid\" to test the refusal).");
      };
      dz("center", 0.5);
      dz("range", 0.1);
    } else if (step.value !== undefined) {
      add(at + a + " does not take a \"value\".");
    }
    if (a === "flash") {
      if (step.version !== undefined && (typeof step.version !== "string" || step.version.length < 1 || step.version.length > 31)) {
        add(at + "flash \"version\" must be text of 1 to 31 characters.");
      }
    } else if (step.version !== undefined) {
      add(at + a + " does not take a \"version\".");
    }
  }

  function validateSimExpect(e, at, add) {
    if (!isObj(e) || Object.keys(e).length === 0) { add(at + "\"expect\" needs at least one assertion."); return; }
    const bool = (k) => { if (typeof e[k] !== "boolean") add(at + "expect." + k + " must be true or false."); };
    const oneOf = (k, list) => { if (!list.includes(e[k])) add(at + "expect." + k + " must be one of " + list.join(", ") + "."); };
    for (const k of Object.keys(e)) {
      switch (k) {
        case "calibrated": case "sensor_fault": case "sensor_observed": case "fw_pending": case "neutral": bool(k); break;
        case "center_max": if (!isNum(e[k]) || e[k] < 0 || e[k] > 10) add(at + "expect.center_max must be a number from 0 to 10."); break;
        case "fw_version": if (typeof e[k] !== "string" || e[k].length === 0) add(at + "expect.fw_version must be text."); break;
        case "xac_x": case "xac_y": if (!isNum(e[k]) || e[k] < -1 || e[k] > 1) add(at + "expect." + k + " must be a number from -1 to 1."); break;
        case "mouse_moved": oneOf(k, ["right", "left", "up", "down", "none"]); break;
        case "clicked": oneOf(k, ["left", "right", "none"]); break;
        case "mode": oneOf(k, ["mouse", "xac"]); break;
        case "fw_slot": oneOf(k, ["ota_0", "ota_1"]); break;
        default: add(at + "unknown expect key \"" + k + "\". Known: " + SIM_EXPECT_KEYS.join(", ") + ".");
      }
    }
  }

  // Accumulates hid telemetry for the output view. Mouse mode sums dx, dy and
  // the wheel, keeps a position that wraps inside width x height, and counts
  // button presses (rising edges of bit 0 left, bit 1 right). XAC mode keeps
  // the latest x and y. apply() returns { clicked: ["left", ...] } for the
  // presses in that message, or null when the message is not hid telemetry.
  function createHidAccumulator(opts) {
    const width = (opts && opts.width) || 200, height = (opts && opts.height) || 200;
    const wrap = (v, n) => ((v % n) + n) % n;
    const acc = {
      mode: "", totalDx: 0, totalDy: 0, wheel: 0, buttons: 0,
      leftClicks: 0, rightClicks: 0, lastClick: "", x: 0, y: 0,
      posX: width / 2, posY: height / 2,
      reset() {
        acc.totalDx = 0; acc.totalDy = 0; acc.wheel = 0; acc.buttons = 0;
        acc.leftClicks = 0; acc.rightClicks = 0; acc.lastClick = "";
        acc.x = 0; acc.y = 0; acc.posX = width / 2; acc.posY = height / 2;
      },
      apply(msg) {
        if (!msg || msg.type !== "hid") return null;
        acc.mode = msg.mode;
        const clicked = [];
        if (msg.mode === "xac") { acc.x = msg.x; acc.y = msg.y; return { clicked }; }
        acc.totalDx += msg.dx; acc.totalDy += msg.dy; acc.wheel += msg.wheel;
        acc.posX = wrap(acc.posX + msg.dx, width);
        acc.posY = wrap(acc.posY + msg.dy, height);
        const rose = msg.buttons & ~acc.buttons;
        if (rose & 1) { acc.leftClicks++; clicked.push("left"); acc.lastClick = "left"; }
        if (rose & 2) { acc.rightClicks++; clicked.push("right"); acc.lastClick = "right"; }
        acc.buttons = msg.buttons;
        return { clicked };
      },
    };
    return acc;
  }

  const OmegaProtocol = {
    SIM_PATTERNS, SIM_FAULTS, SIM_NEXT_IMAGE,
    encodeSimMessage, parseSimMessage, validateSimScript, createHidAccumulator,
    pickReconnectCandidate,
    FEED_URL, RELEASES_URL, feedUrlFor, parseReleaseFeed, summarizeAvailability,
    OTA_SLOT_SIZE, CHIP_ID_ESP32S3, DEFAULT_PROJECT, KIND_LABEL,
    inspectImage, parseVersion, compareFirmware, buildFlashPlan, buildRevertPlan,
    decideOutcome, CONFIRM_CHECKS, judgeConfirmCheck, awaitSensorHealthy, runConfirmCheck, confirmFailureNotice,
    REVERTIBLE_STATES, PARTIAL_SLOT_NOTE, revertAvailability, describePrevState,
    RECONNECT_WINDOW_MS, updateActive, resumesOnReconnect, reconnectDeadline, reconnectWaitText,
    PENDING_FLASH_REASON, updateControls, afterLinkLoss, decideAfterFinishTimeout, shouldWarnBeforeUnload,
    classifyFetchError, describeFeedFailure, describeDownloadFailure,
    SENSOR_NOT_OBSERVED, SENSOR_POLL_MS, SENSOR_WAIT_MS, SENSOR_DWELL_MS, describeSensorState,
    describeUpdateError, formatProgress, formatBytes,
    FIRST_FLASH_URL, NO_USB_UPDATES_NOTE, usbUpdatesUnavailable, decideOutcomeNoUpdates,
    HEADER_SIZE, CRC_SIZE, MAX_PAYLOAD, MAX_FRAME,
    MODULE_CALIBRATION, MODULE_DISCOVERY, T_LIST_MODULES, PROTOCOL_ID, PROTOCOL_VERSION,
    FLAGS_REQUEST, FLAGS_REPLY_BIT, FLAG_CORRELATION,
    TYPE, INPUT_MODE,
    crc32, buildFrame, FrameParser, readU32,
    parseErrorReply, parseDiscovery, selectCalibrationModule, decodeState, decodeDiagnostics,
    encodeSetManualOffset, encodeSetDeadzoneOverride,
    encodeSetNudgeSettings, encodeSetInputMode,
  };
  globalThis.OmegaProtocol = OmegaProtocol;
  if (typeof module !== "undefined" && module.exports) module.exports = OmegaProtocol;
})();
