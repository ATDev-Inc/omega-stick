"use strict";
// Smoke test for tests/web/mock_sim.mjs, the stand-in for omega_sim used to
// try the console's Simulator connection. Self-contained: ephemeral port on
// 127.0.0.1, Node's built-in WebSocket client, a few hundred milliseconds.

const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");
const { pathToFileURL } = require("node:url");

const P = require(path.join(__dirname, "..", "..", "components", "omega_calibration", "web", "omega_protocol.js"));

test("mock simulator speaks hello, stick, patterns and binary frames", async () => {
  const { startMockSim } = await import(pathToFileURL(path.join(__dirname, "mock_sim.mjs")).href);
  const sim = await startMockSim({ port: 0 });
  const ws = new WebSocket("ws://127.0.0.1:" + sim.port);
  ws.binaryType = "arraybuffer";
  const texts = [];
  const bins = [];
  ws.onmessage = (e) => (typeof e.data === "string" ? texts.push(P.parseSimMessage(e.data)) : bins.push(new Uint8Array(e.data)));
  try {
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = () => reject(new Error("connect failed")); });
    ws.send(P.encodeSimMessage({ type: "hello" }));
    ws.send(JSON.stringify({ type: "drift", rate: 11 }));
    ws.send(P.encodeSimMessage({ type: "stick", x: 0.5, y: -0.25 }));
    ws.send(P.encodeSimMessage({ type: "pattern", name: "nudge_right" }));
    ws.send(P.buildFrame(P.MODULE_CALIBRATION, P.TYPE.GET_STATE));
    const deadline = Date.now() + 3000;
    while (Date.now() < deadline && !(bins.length && texts.some((m) => m && m.type === "hid" && m.buttons === 1))) {
      await new Promise((r) => setTimeout(r, 20));
    }
    const types = texts.map((m) => m && m.type);
    assert.ok(types.includes("hello"));
    assert.ok(types.includes("sensor"));
    assert.equal(types.filter((t) => t === "hello").length, 1, "hello is sent once, on connect");
    assert.ok(texts.some((m) => m && m.type === "error"), "out-of-range drift is refused");
    const hid = texts.find((m) => m && m.type === "hid" && m.dx !== 0);
    assert.deepEqual([hid.dx, hid.dy], [4, 2]);
    assert.ok(texts.some((m) => m && m.type === "source" && m.kind === "pattern" && m.name === "nudge_right"));
    assert.equal(bins.length, 1);
    const frames = new P.FrameParser().feed(bins[0]).frames;
    assert.equal(frames.length, 1);
    assert.equal(frames[0].type, 0xD0);
  } finally {
    ws.close();
    await sim.stop();
  }
});

test("mock sim: SET_POSITION_STREAM gets OK then progress frames; FINISH_CALIBRATION is refused", async () => {
  const { startMockSim } = await import(pathToFileURL(path.join(__dirname, "mock_sim.mjs")).href);
  const sim = await startMockSim({ port: 0 });
  const bins = [];
  const ws = new WebSocket("ws://127.0.0.1:" + sim.port);
  ws.binaryType = "arraybuffer";
  ws.onmessage = (e) => { if (typeof e.data !== "string") bins.push(new Uint8Array(e.data)); };
  try {
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = () => reject(new Error("connect failed")); });
    ws.send(P.buildSetPositionStream(3000));
    ws.send(P.buildFrame(P.MODULE_CALIBRATION, P.TYPE.FINISH_CALIBRATION));
    const parser = new P.FrameParser();
    const frames = [];
    const deadline = Date.now() + 3000;
    while (Date.now() < deadline && frames.length < 5) {
      for (const b of bins.splice(0)) frames.push(...parser.feed(b).frames);
      await new Promise((r) => setTimeout(r, 20));
    }
    assert.deepEqual(frames.map((f) => f.type), [P.TYPE.OK, 0xD4, 0xD4, 0xD4, P.TYPE.ERROR]);
    const pr = P.parseCalibrationProgress(frames[1].payload);
    assert.equal(pr.stage, 0); assert.equal(pr.sectorCount, 32); assert.equal(pr.rawValid, true);
    assert.deepEqual(sim.seen.streams, [3000]);
    assert.equal(sim.seen.finishes, 1);
  } finally {
    ws.close();
    await sim.stop();
  }
});
