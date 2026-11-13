// A tiny stand-in for omega_sim, for exercising the calibration console's
// Simulator connection without the real simulator. Dev/test aid only; it is
// not shipped and speaks just enough of docs/simulator.md:
//
//   text   hello is sent on connect (a page hello is answered with nothing,
//          as in docs/simulator.md); stick -> sensor + hid; pattern/stop -> source;
//          fault/next_image/script are recorded; drift and noise outside
//          their limits get an error; reset/wipe -> rebooting + close
//   binary GET_STATE -> the golden STATE reply; GET_FW_INFO -> the running
//          firmware; OTA_BEGIN/DATA/FINISH -> OK, then the image's version
//          becomes the running one after rebooting ("update") + close;
//          RESTART -> OK, then rebooting + close
//          SET_POSITION_STREAM -> OK, then CALIBRATION_PROGRESS stage 0 frames
//          (silent when sim.progress.unsupported, like firmware before protocol v2);
//          FINISH_CALIBRATION -> ERROR operation_not_permitted (no sweep runs)
//          GET_DIAGNOSTICS -> DIAGNOSTICS with sensor_fault from sim.diag.sensorFault
//          (settable); MARK_VALID -> OK and pending becomes false (an upload sets
//          pending, as a new image boots pending verify)
//
// Only Node built-ins: the WebSocket upgrade and framing are done by hand.
//
// Standalone:  node tests/web/mock_sim.mjs [port] [--log file.json]
// As a module: const sim = await startMockSim({ port: 0 }); sim.port; sim.received; await sim.stop();

import http from "node:http";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const P = require(path.join(here, "..", "..", "components", "omega_calibration", "web", "omega_protocol.js"));

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function loadGolden() {
  try {
    const g = JSON.parse(fs.readFileSync(path.join(here, "..", "protocol", "golden_frames.json"), "utf8"));
    const state = g.replies.find((r) => r.name === "GET_STATE");
    return state ? Uint8Array.from(Buffer.from(state.frame_hex, "hex")) : null;
  } catch (_) { return null; }
}

// A device to host frame: same layout as buildFrame but with the reply bit set.
function replyFrame(module, type, payload) {
  const f = P.buildFrame(module, type, payload);
  f[2] = P.FLAGS_REQUEST | P.FLAGS_REPLY_BIT;
  const crc = P.crc32(f.subarray(0, f.length - P.CRC_SIZE));
  new DataView(f.buffer).setUint32(f.length - P.CRC_SIZE, crc, true);
  return f;
}

function fwInfoPayload(fw) {
  const strs = [fw.project, fw.version, "Oct 10 2026", "14:03:11", "v6.0.2", "ota_0", fw.sha, "ota_0", "ota_1", "", "", "", "empty"];
  const parts = [Buffer.from([fw.pending ? 1 : 0, 0])];
  for (const s of strs) { const b = Buffer.from(s, "utf8"); parts.push(Buffer.from([b.length]), b); }
  return new Uint8Array(Buffer.concat(parts));
}

// u8 sensor_fault, u8 sensor_first_fault, u32 sensor_fault_count, u32 boot_count, u8 reset_count (0).
function diagPayload(fault) {
  const b = Buffer.alloc(11);
  b[0] = fault; b[1] = fault; b.writeUInt32LE(fault ? 1 : 0, 2); b.writeUInt32LE(1, 6);
  return new Uint8Array(b);
}
// A stage 0 CALIBRATION_PROGRESS payload (179 bytes) with the dot at (0.01 * i, 0).
function progressPayload(i) {
  const b = Buffer.alloc(179);
  b[0] = 0; b[1] = 0b0011;
  b.writeFloatLE(0.01 * i, 10);
  b.writeUInt32LE(0, 46);
  b[50] = 32;
  return new Uint8Array(b);
}
function errorFrame(code, msg) {
  const m = Buffer.from(msg, "utf8");
  const p = Buffer.alloc(4 + m.length);
  p.writeUInt32LE(code, 0); m.copy(p, 4);
  return replyFrame(5, P.TYPE.ERROR, new Uint8Array(p));
}

// Version and short sha from an ESP32 app image's descriptor (first segment).
function imageFw(img) {
  const d = 32;
  const cstr = (off, n) => Buffer.from(img.subarray(off, off + n)).toString("utf8").replace(/\0.*$/s, "");
  return { project: cstr(d + 48, 32), version: cstr(d + 16, 32), sha: Buffer.from(img.subarray(d + 144, d + 148)).toString("hex") };
}

function wsFrame(opcode, payload) {
  const len = payload.length;
  let head;
  if (len < 126) head = Buffer.from([0x80 | opcode, len]);
  else if (len < 65536) { head = Buffer.alloc(4); head[0] = 0x80 | opcode; head[1] = 126; head.writeUInt16BE(len, 2); }
  else { head = Buffer.alloc(10); head[0] = 0x80 | opcode; head[1] = 127; head.writeBigUInt64BE(BigInt(len), 2); }
  return Buffer.concat([head, payload]);
}

export function startMockSim({ port = 0, host = "127.0.0.1", logFile = "" } = {}) {
  const goldenState = loadGolden();
  const received = [];      // every text message the page sent (parsed)
  const binaryIn = [];      // frames the page sent: { module, type }
  let client = null;        // { socket, ... } the current connection
  let connections = 0;
  const state = { x: 0, y: 0, mode: "mouse", timer: null };
  const fw = { project: "omega_stick", version: "0.1.0+sim", sha: "00000000", pending: false };
  const diag = { sensorFault: 0, unsupported: false }; // settable by tests
  const seen = { markValid: 0, diagnostics: 0, streams: [], finishes: 0 };
  const progress = { unsupported: false }; // settable: older firmware never answers SET_POSITION_STREAM
  let ota = null;           // image bytes received so far during an upload

  const flush = () => {
    if (!logFile) return;
    try { fs.writeFileSync(logFile, JSON.stringify({ connections, received, binaryIn }, null, 1)); } catch (_) { }
  };

  const sendText = (c, obj) => { if (c.socket.writable) c.socket.write(wsFrame(0x1, Buffer.from(JSON.stringify(obj)))); };
  const sendBin = (c, bytes) => { if (c.socket.writable) c.socket.write(wsFrame(0x2, Buffer.from(bytes))); };
  const closeClient = (c) => {
    try { c.socket.write(wsFrame(0x8, Buffer.alloc(0))); } catch (_) { }
    setTimeout(() => c.socket.destroy(), 20);
  };

  function emitMotion(c) {
    const dx = Math.round(state.x * 8), dy = Math.round(-state.y * 8);
    sendText(c, { type: "sensor", x: state.x, y: state.y, bx: +(state.x * 20).toFixed(2), by: +(state.y * 20).toFixed(2), ok: true, fault: "ok" });
    if (state.mode === "xac") sendText(c, { type: "hid", mode: "xac", x: state.x, y: state.y });
    else if (dx || dy) sendText(c, { type: "hid", mode: "mouse", dx, dy, wheel: 0, buttons: 0 });
  }

  function rebootClient(c, reason) {
    sendText(c, { type: "rebooting", reason });
    closeClient(c);
  }

  function onText(c, text) {
    let m;
    try { m = JSON.parse(text); } catch (_) { return; }
    if (!m || typeof m.type !== "string") return;
    received.push(m);
    flush();
    switch (m.type) {
      case "hello": break; // the real simulator sends its hello on connect and answers this with nothing
      case "stick":
        state.x = Math.max(-1, Math.min(1, +m.x || 0)); state.y = Math.max(-1, Math.min(1, +m.y || 0));
        sendText(c, { type: "source", kind: "manual", name: "", t_ms: 0, done: false });
        emitMotion(c);
        break;
      case "pattern":
        sendText(c, { type: "source", kind: "pattern", name: String(m.name), t_ms: 0, done: false });
        if (String(m.name).startsWith("nudge")) {
          sendText(c, { type: "hid", mode: "mouse", dx: 0, dy: 0, wheel: 1, buttons: 1 });
          setTimeout(() => sendText(c, { type: "hid", mode: "mouse", dx: 0, dy: 0, wheel: 0, buttons: 0 }), 150);
        }
        break;
      case "stop":
        state.x = 0; state.y = 0;
        sendText(c, { type: "source", kind: "manual", name: "", t_ms: 0, done: false });
        break;
      case "script":
        sendText(c, { type: "source", kind: "script", name: "", t_ms: 0, done: false });
        break;
      case "reset": rebootClient(c, "reset"); break;
      case "wipe": rebootClient(c, "wipe"); break;
      case "drift":
        if (!(m.rate >= 0 && m.rate <= 10)) sendText(c, { type: "error", message: "drift rate must be 0 to 10" });
        break;
      case "noise":
        if (!(m.sigma >= 0 && m.sigma <= 1)) sendText(c, { type: "error", message: "noise sigma must be 0 to 1" });
        break;
      default: break; // fault, drift, noise, next_image: recorded only
    }
  }

  function onBinary(c, data) {
    c.parser = c.parser || new P.FrameParser();
    const { frames } = c.parser.feed(new Uint8Array(data));
    for (const f of frames) {
      binaryIn.push({ module: f.module, type: f.type });
      flush();
      if (f.module === 5 && f.type === P.TYPE.GET_STATE && goldenState) sendBin(c, goldenState);
      else if (f.module === 5 && f.type === P.TYPE.GET_FW_INFO) sendBin(c, replyFrame(5, P.TYPE.FW_INFO, fwInfoPayload(fw)));
      else if (f.module === 5 && f.type === P.TYPE.OTA_BEGIN) { ota = []; sendBin(c, replyFrame(5, 0xDF, new Uint8Array(4))); }
      else if (f.module === 5 && f.type === P.TYPE.OTA_DATA && ota) {
        ota.push(Buffer.from(f.payload.subarray(4)));
        const total = new Uint8Array(4);
        new DataView(total.buffer).setUint32(0, ota.reduce((n, b) => n + b.length, 0), true);
        sendBin(c, replyFrame(5, 0xDF, total));
      } else if (f.module === 5 && f.type === P.TYPE.OTA_FINISH && ota) {
        Object.assign(fw, imageFw(new Uint8Array(Buffer.concat(ota))), { pending: true });
        ota = null;
        sendBin(c, replyFrame(5, 0xDF, new Uint8Array(4)));
        setTimeout(() => rebootClient(c, "update"), 100);
      } else if (f.module === 5 && f.type === P.TYPE.MARK_VALID) {
        seen.markValid++;
        fw.pending = false;
        sendBin(c, replyFrame(5, 0xDF, new Uint8Array(4)));
      } else if (f.module === 5 && f.type === P.TYPE.GET_DIAGNOSTICS) {
        seen.diagnostics++;
        sendBin(c, diag.unsupported ? errorFrame(88, "GET_DIAGNOSTICS: Function not implemented") : replyFrame(5, P.TYPE.DIAGNOSTICS, diagPayload(diag.sensorFault)));
      } else if (f.module === 5 && f.type === P.TYPE.SET_POSITION_STREAM) {
        const lease = f.payload.length >= 4 ? new DataView(f.payload.buffer, f.payload.byteOffset).getUint32(0, true) : 0;
        seen.streams.push(lease);
        if (!progress.unsupported) {
          sendBin(c, replyFrame(5, 0xDF, new Uint8Array(4)));
          if (lease > 0) for (let i = 0; i < 3; i++) sendBin(c, replyFrame(5, P.TYPE.CALIBRATION_PROGRESS, progressPayload(i)));
        }
      } else if (f.module === 5 && f.type === P.TYPE.FINISH_CALIBRATION) {
        seen.finishes++;
        sendBin(c, errorFrame(1, "no range sweep is running"));
      } else if (f.module === 5 && f.type === P.TYPE.RESTART) {
        sendBin(c, replyFrame(5, 0xDF, new Uint8Array(4)));
        setTimeout(() => rebootClient(c, "restart"), 100);
      }
    }
  }

  function onData(c, chunk) {
    c.buf = Buffer.concat([c.buf, chunk]);
    for (;;) {
      const b = c.buf;
      if (b.length < 2) return;
      const opcode = b[0] & 0x0f;
      const masked = (b[1] & 0x80) !== 0;
      let len = b[1] & 0x7f, off = 2;
      if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); off = 10; }
      const need = off + (masked ? 4 : 0) + len;
      if (b.length < need) return;
      let payload = b.subarray(off + (masked ? 4 : 0), need);
      if (masked) {
        const key = b.subarray(off, off + 4);
        payload = Buffer.from(payload.map((v, i) => v ^ key[i % 4]));
      }
      c.buf = b.subarray(need);
      if (opcode === 0x1) onText(c, payload.toString("utf8"));
      else if (opcode === 0x2) onBinary(c, payload);
      else if (opcode === 0x8) { closeClient(c); return; }
      else if (opcode === 0x9) c.socket.write(wsFrame(0xA, payload));
    }
  }

  const server = http.createServer((req, res) => { res.writeHead(426); res.end("WebSocket only"); });
  server.on("upgrade", (req, socket) => {
    const key = req.headers["sec-websocket-key"];
    if (!key) { socket.destroy(); return; }
    const accept = crypto.createHash("sha1").update(key + GUID).digest("base64");
    socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
      "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
    if (client) { const old = client; client = null; closeClient(old); } // one client at a time
    const c = { socket, buf: Buffer.alloc(0), parser: null };
    client = c;
    connections++;
    sendText(c, { type: "hello", sim: "omega_sim", version: 1, fw: "0.1.0+sim", mode: state.mode, slot: "ota_0" });
    socket.on("data", (d) => onData(c, d));
    socket.on("error", () => { });
    socket.on("close", () => { if (client === c) client = null; });
    flush();
  });

  return new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, host, () => {
      resolve({
        port: server.address().port,
        received, binaryIn, fw, diag, seen, progress,
        get connections() { return connections; },
        stop() {
          clearInterval(state.timer);
          if (client) client.socket.destroy();
          return new Promise((r) => { server.close(() => r()); server.closeAllConnections && server.closeAllConnections(); });
        },
      });
    });
  });
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  const li = args.indexOf("--log");
  const logFile = li >= 0 ? args.splice(li, 2)[1] : "";
  const port = args[0] ? Number(args[0]) : 8765;
  const sim = await startMockSim({ port, logFile });
  console.log("mock omega_sim listening on ws://127.0.0.1:" + sim.port);
}
