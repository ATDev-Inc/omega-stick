// End-to-end check: the real omega_sim driven through the real
// calibration console in headless Chrome. Runs in CI (e2e_tests.yml). See README.md.
//
//   node tests/e2e/sim_console.mjs [--sim path/to/omega_sim] [--chrome path/to/chrome] [--speed 4] [--no-sandbox] [--verbose]
//
// Needs only Node 24 (global WebSocket and fetch) and Chrome.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { pathToFileURL } from "node:url";
import { freePort, sleep, launchChrome, connectCdp } from "./cdp.mjs";

const root = path.resolve(import.meta.dirname, "..", "..");
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf("--" + name); return i >= 0 ? args[i + 1] : def; };
const verbose = args.includes("--verbose");
const noSandbox = args.includes("--no-sandbox");
const exe = process.platform === "win32" ? ".exe" : "";
const simPath = opt("sim", path.join(root, "build-host", "sim", "omega_sim" + exe));
const chromePath = opt("chrome", process.platform === "win32"
  ? "C:/Program Files/Google/Chrome/Application/chrome.exe"
  : process.platform === "darwin" ? "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" : "google-chrome");
const speed = opt("speed", "4");

// ---- a synthetic but valid ESP32-S3 app image (same layout as tests/web/update.test.js) ----
function makeImage(version) {
  const putStr = (b, off, s) => { for (let i = 0; i < s.length; i++) b[off + i] = s.charCodeAt(i); };
  const dataLen = 320, pos = 24 + 8 + dataLen, total = (pos | 15) + 1 + 32;
  const b = new Uint8Array(total), v = new DataView(b.buffer);
  b[0] = 0xE9; b[1] = 1; b[2] = 2; b[3] = 0x4F;
  v.setUint32(4, 0x40375000, true); v.setUint16(12, 9, true); v.setUint16(17, 99, true); b[23] = 1;
  v.setUint32(24, 0x3C000000, true); v.setUint32(28, dataLen, true);
  const d = 32;
  v.setUint32(d, 0xABCD5432, true);
  putStr(b, d + 16, version); putStr(b, d + 48, "omega_stick"); putStr(b, d + 80, "12:34:56");
  putStr(b, d + 96, "Oct 10 2026"); putStr(b, d + 112, "v6.0.2");
  for (let k = 0; k < 32; k++) b[d + 144 + k] = (0xAB + k + version.length) & 0xFF;
  return b;
}

// ---- results ----
const results = [];
const check = (id, name, ok, evidence) => {
  results.push({ id, name, ok: !!ok, evidence });
  console.log((ok ? "PASS " : "FAIL ") + id + " " + name + (evidence ? "  [" + evidence + "]" : ""));
};

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "omega-e2e-"));
let sim = null, chrome = null, cdp = null, simOut = "";

async function main() {
  const port = await freePort();
  const inject = "<head><script>localStorage.setItem('omegaConsoleSimUrl','ws://127.0.0.1:" + port + "');" +
    "window.__errs=[];addEventListener('error',e=>__errs.push(String(e.message)));" +
    "addEventListener('unhandledrejection',e=>__errs.push('rejection: '+String(e.reason)));" +
    "const ce=console.error;console.error=function(){__errs.push('console.error: '+[].join.call(arguments,' '));ce.apply(console,arguments)};</script>";
  const html = fs.readFileSync(path.join(root, "components/omega_calibration/web/calibration_console.html"), "utf8")
    .replace("<head>", () => inject);
  fs.writeFileSync(path.join(tmp, "page.html"), html);

  chrome = await launchChrome(chromePath, path.join(tmp, "chrome"), "about:blank", noSandbox ? ["--no-sandbox"] : []);
  cdp = await connectCdp(chrome.wsUrl, (m) => {
    if (m.method === "Page.javascriptDialogOpening") {
      if (verbose) console.log("  dialog accepted: " + m.params.message.split("\n")[0]);
      cdp.send("Page.handleJavaScriptDialog", { accept: true }).catch(() => { });
    }
  });
  await cdp.send("Page.enable"); await cdp.send("Runtime.enable"); await cdp.send("DOM.enable");
  await cdp.send("Page.navigate", { url: pathToFileURL(path.join(tmp, "page.html")).href });
  await sleep(1200);

  const ev = async (expr) => {
    const r = await cdp.send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error("page eval failed: " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
    return r.result.value;
  };
  const $ = (id) => "document.getElementById('" + id + "')";
  const text = (id) => ev($(id) + ".textContent");
  const click = (id) => ev($(id) + ".click()");
  const logText = () => ev("document.getElementById('log').innerText");
  const waitFor = async (label, fn, timeout = 30000) => {
    const end = Date.now() + timeout;
    for (;;) {
      const v = await fn();
      if (v) return v;
      if (Date.now() > end) throw new Error("timed out waiting for " + label);
      await sleep(100);
    }
  };
  const waitLog = (label, re, timeout) => waitFor(label, async () => re.test(await logText()), timeout);
  const countIn = async (s) => (await logText()).split(s).length - 1;
  const setValue = (id, v) => ev("(()=>{const e=" + $(id) + ";e.value=" + JSON.stringify(String(v)) + ";e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}))})()");
  const setChecked = (id, on) => ev("(()=>{const e=" + $(id) + ";if(e.checked!==" + on + ")e.click()})()");
  const KEYCODE = { ArrowRight: 39, ArrowUp: 38, ArrowLeft: 37, ArrowDown: 40, Home: 36 };
  const key = async (k, n = 1) => {
    for (let i = 0; i < n; i++) {
      await cdp.send("Input.dispatchKeyEvent", { type: "keyDown", key: k, code: k, windowsVirtualKeyCode: KEYCODE[k] });
      await cdp.send("Input.dispatchKeyEvent", { type: "keyUp", key: k, code: k, windowsVirtualKeyCode: KEYCODE[k] });
      await sleep(50);
    }
  };
  const focusPad = () => ev($("simPad") + ".focus()");
  const pattern = (name) => ev("document.querySelector('[data-pattern=" + name + "]').click()");
  const mouseTotals = async () => {
    const m = /moved (-?\d+) across and (-?\d+) down/.exec(await text("simOutText"));
    return m ? { dx: +m[1], dy: +m[2] } : null;
  };
  const gamepad = async () => {
    const m = /x (-?\d+\.\d+), y (-?\d+\.\d+)/.exec(await text("simOutText"));
    return m ? { x: +m[1], y: +m[2] } : null;
  };
  const connectedCount = () => countIn("Connected (simulator over WebSocket)");

  // ---- a. connect, hello, panel, first-boot prompts, calibrate ----
  // The page is loaded first and the simulator started afterwards, so the page
  // can join before the first-boot sweep has printed its first prompt.
  sim = spawn(simPath, ["--fresh", "--data", path.join(tmp, "simdata"), "--port", String(port), "--speed", speed], { stdio: ["ignore", "pipe", "pipe"] });
  sim.stdout.on("data", (d) => { simOut += d; });
  sim.stderr.on("data", (d) => { simOut += d; });
  let simExit = null;
  sim.on("exit", (c, s) => { simExit = { code: c, signal: s }; });
  await waitFor("the simulator to listen", () => /listening on/.test(simOut), 10000);
  await waitFor("connect", async () => {
    await click("simBtn");
    await sleep(150);
    return (await text("simBtn")) === "Disconnect";
  }, 10000);
  await waitFor("hello", async () => /Connected to omega_sim/.test(await text("simInfo")), 5000);
  const info = await text("simInfo");
  const panelShown = await ev("!" + $("simPanel") + ".hidden");
  await waitLog("the range calibration prompt", /RANGE CALIBRATION/, 20000);
  const logA = await logText();
  await pattern("calibrate");
  await waitLog("calibration results", /CALIBRATION RESULTS/, 40000);
  await waitLog("state refresh after the sweep", /Recalibration finished successfully/, 10000);
  await sleep(500);
  const stateInfo = await text("stateInfo");
  const centerField = await ev("document.querySelector('[data-f=center]').textContent");
  const radiusField = await ev("document.querySelector('[data-f=usableRadius]').textContent");
  const calibrated = /[0-9]/.test(centerField) && /[0-9]/.test(radiusField);
  check("a", "connect, hello, panel, first-boot prompts, calibrate pattern, state shows a calibration",
    panelShown && /Connected to omega_sim/.test(info) && /Move the joystick slowly/.test(logA) &&
    calibrated && (await countIn("Fetched calibration & settings state")) >= 2,
    info + " | log has the range prompts: " + /Move the joystick slowly/.test(logA) + " | state: " + stateInfo + " | center " + centerField + ", usable radius " + radiusField);
  await click("simStopBtn");

  // ---- b. virtual stick moves the mouse; nudge clicks ----
  await focusPad();
  await click("simClearOutBtn");
  const b0 = (await mouseTotals()) || { dx: 0, dy: 0 };
  await key("ArrowRight", 10);
  await waitFor("rightward mouse motion", async () => ((await mouseTotals()) || b0).dx > b0.dx + 3, 8000).catch(() => { });
  const b1 = await mouseTotals();
  await key("Home");
  await sleep(500);
  await click("simClearOutBtn");
  // pointer: press near the top of the pad (screen up)
  await ev($("simPad") + ".scrollIntoView({block:'center'})");
  const rect = await ev("(()=>{const r=" + $("simPad") + ".getBoundingClientRect();return {x:r.left+r.width/2,y:r.top+r.height/2,w:r.width}})()");
  const reach = rect.w / 2 - 18;
  const upY = rect.y - 0.6 * reach;
  await cdp.send("Input.dispatchMouseEvent", { type: "mousePressed", x: rect.x, y: upY, button: "left", buttons: 1, clickCount: 1 });
  await sleep(100);
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: rect.x, y: upY, button: "left", buttons: 1 });
  await waitFor("upward mouse motion", async () => ((await mouseTotals()) || { dy: 0 }).dy < -3, 8000).catch(() => { });
  const b2 = await mouseTotals();
  const stickText = await text("simStickText");
  await cdp.send("Input.dispatchMouseEvent", { type: "mouseReleased", x: rect.x, y: upY, button: "left", buttons: 0, clickCount: 1 });
  await sleep(500);
  await click("simClearOutBtn");
  await pattern("nudge_right");
  await waitFor("a click indicator", async () => /Last click: (left|right)/.test(await text("simLastClick")), 8000).catch(() => { });
  const click1 = await text("simLastClick");
  check("b", "stick right moves the dot right, pushing up moves it up (negative dy), a nudge clicks",
    b1 && b1.dx > b0.dx + 3 && b2 && b2.dy < -3 && /Last click: (left|right)/.test(click1),
    "right: dx " + b0.dx + " -> " + (b1 && b1.dx) + "; up: " + stickText + ", totals " + JSON.stringify(b2) + "; nudge: " + click1);
  await sleep(1500);
  await click("simStopBtn");

  // ---- c. recalibrate from the console button while playing the calibrate pattern ----
  const cBefore = await countIn("Recalibration finished successfully");
  await click("recalibrateBtn");
  const samples = [];
  const sample = async () => samples.push(await ev($("recalibrateBtn") + ".disabled"));
  await waitFor("recalibration accepted", async () => { await sample(); return /Recalibration accepted/.test(await logText()); }, 8000);
  await pattern("calibrate");
  const cEnd = Date.now() + 40000;
  while (Date.now() < cEnd && (await countIn("Recalibration finished successfully")) === cBefore) {
    await sample(); await sleep(150);
  }
  await sleep(300);
  const recalInfo = await text("recalInfo");
  const enabledAfter = !(await ev($("recalibrateBtn") + ".disabled"));
  check("c", "recalibrate button stays disabled until the result, result shown",
    samples.length > 3 && samples.every(Boolean) && /Finished successfully/.test(recalInfo) && enabledAfter,
    samples.length + " samples, all disabled: " + samples.every(Boolean) + "; recalInfo: " + recalInfo + "; enabled after: " + enabledAfter);
  await click("simStopBtn");

  // ---- d. deadzone override: in range works, out of range refused before sending ----
  await setChecked("centerDzEnable", true);
  await setValue("centerDzValue", "0.1");
  await click("applyDeadzoneBtn");
  await waitLog("deadzone applied", /Deadzone override applied live/, 8000);
  const appliedOnce = await countIn("Deadzone override applied live");
  const dzCenter = await ev($("centerDzValue") + ".value");
  await setValue("centerDzValue", "0.9");
  await click("applyDeadzoneBtn");
  await sleep(500);
  const logD = await logText();
  const refused = /Center deadzone override must be between 0 and 0\.5/.test(logD);
  const appliedAfter = await countIn("Deadzone override applied live");
  check("d", "in-range deadzone override applied, 0.9 refused by the console before sending",
    refused && appliedAfter === appliedOnce && appliedOnce >= 1,
    "applied x" + appliedOnce + " (value after refresh " + dzCenter + "); refused: " + refused + "; applied count unchanged: " + (appliedAfter === appliedOnce));
  await setChecked("centerDzEnable", false);
  await click("applyDeadzoneBtn");
  await waitFor("override cleared", async () => (await countIn("Deadzone override applied live")) > appliedOnce, 8000);

  // ---- e. XAC mode: the sim restarts, the page reconnects by itself ----
  const connBefore = await connectedCount();
  await click("modeXacBtn");
  await waitFor("automatic reconnect", async () => (await connectedCount()) > connBefore, 30000);
  await waitFor("hello in xac mode", async () => /input mode xac/.test(await text("simInfo")), 8000);
  await focusPad();
  await key("ArrowRight", 10);
  await waitFor("gamepad x", async () => ((await gamepad()) || { x: 0 }).x > 0.2, 8000).catch(() => { });
  const gpRight = await gamepad();
  await key("ArrowUp", 10);
  await waitFor("gamepad y", async () => ((await gamepad()) || { y: 0 }).y > 0.2, 8000).catch(() => { });
  const gpUp = await gamepad();
  const logE = await logText();
  check("e", "XAC mode: sim restarts, page reconnects by itself, crosshair follows the stick",
    /restarting \(mode switch\)/.test(logE) && gpRight && gpRight.x > 0.2 && gpUp && gpUp.y > 0.2,
    "reconnects " + connBefore + " -> " + (await connectedCount()) + "; " + (await text("simInfo")) + "; right " + JSON.stringify(gpRight) + ", up " + JSON.stringify(gpUp));

  // ---- f. sensor fault ----
  await setValue("simFault", "no_response");
  await click("simApplyBtn");
  await waitFor("neutral output", async () => { const g = await gamepad(); return g && g.x === 0 && g.y === 0; }, 8000).catch(() => { });
  const gpFault = await gamepad();
  await sleep(500);
  await click("diagRefreshBtn");
  await sleep(800);
  const diagFault = await ev($("diagnosticFields") + ".innerText.replace(/\\s+/g,' ')");
  const senFault = await text("simSenFault");
  await setValue("simFault", "ok");
  await click("simApplyBtn");
  await waitFor("output recovers", async () => ((await gamepad()) || { x: 0 }).x > 0.2, 10000).catch(() => { });
  const gpOk = await gamepad();
  await click("diagRefreshBtn");
  await sleep(800);
  const diagOk = await ev($("diagnosticFields") + ".innerText.replace(/\\s+/g,' ')");
  check("f", "no_response: diagnostics show a fault and output is neutral; back to ok recovers",
    gpFault && gpFault.x === 0 && gpFault.y === 0 && senFault === "no_response" && diagFault !== diagOk && gpOk && gpOk.x > 0.2,
    "fault: gamepad " + JSON.stringify(gpFault) + ", sensor fault " + senFault + ", diag [" + diagFault.slice(0, 160) + "]; ok: gamepad " + JSON.stringify(gpOk) + ", diag [" + diagOk.slice(0, 160) + "]");
  await focusPad(); await key("Home");

  // ---- g. firmware update, revert, rollback ----
  const imgOf = (v) => { const p = path.join(tmp, "image-" + v + ".bin"); fs.writeFileSync(p, makeImage(v)); return p; };
  const flash = async (file) => {
    const { root: docRoot } = await cdp.send("DOM.getDocument");
    const { nodeId } = await cdp.send("DOM.querySelector", { nodeId: docRoot.nodeId, selector: "#fwFile" });
    await cdp.send("DOM.setFileInputFiles", { files: [file], nodeId });
    await waitFor("flash button enabled", async () => !(await ev($("flashBtn") + ".disabled")), 8000);
    await click("flashBtn");
    await waitFor("confirm panel", async () => !(await ev($("confirmPanel") + ".hidden")), 8000);
    await click("confirmOkBtn");
  };
  const fwVersion = () => text("fwVersion");
  const startVer = await fwVersion();
  const connG = await connectedCount();
  await flash(imgOf("0.2.0"));
  await waitFor("the update to be confirmed", async () => /Update confirmed/.test(await text("updateMessage")), 90000);
  await sleep(1000);
  const msgNew = await text("updateMessage");
  // The console checks the new image and confirms it (MARK_VALID); revert and reflash only after that.
  const settled = () => waitFor("the image to be confirmed", async () => ev($("fwPending") + ".hidden"), 40000);
  await settled();
  const reconnectedG = (await connectedCount()) > connG;
  await waitFor("new version shown", async () => (await fwVersion()) === "0.2.0", 10000).catch(() => { });
  const verNew = await fwVersion();
  // revert
  await waitFor("revert button", async () => !(await ev($("fwRevertBtn") + ".disabled")), 15000);
  const connR = await connectedCount();
  await click("fwRevertBtn");
  await waitFor("confirm panel", async () => !(await ev($("confirmPanel") + ".hidden")), 8000);
  await click("confirmOkBtn");
  await waitFor("revert reconnect", async () => (await connectedCount()) > connR, 60000);
  await waitFor("reverted version", async () => (await fwVersion()) === startVer, 30000).catch(() => { });
  await settled();
  const verReverted = await fwVersion();
  const msgRevert = await text("updateMessage");
  // rollback
  await setValue("simNextImage", "fail_self_test");
  await click("simApplyBtn");
  await sleep(300);
  const connF = await connectedCount();
  await flash(imgOf("0.3.0"));
  // The console's sensor check fails, so it refuses to confirm and says so at once. The
  // simulator then rolls the image back at the firmware's 120 s deadline (speed factor
  // applies), reboots into the previous image and the page reconnects by itself.
  await waitFor("the refused confirmation to be reported", async () => /was not confirmed/i.test(await text("updateError")), 60000);
  await sleep(1500);
  const msgRollback = await text("updateError");
  const verWhilePending = await fwVersion();
  // While the refused image waits for its rollback, the link and Flash stay locked
  // (a Disconnect would leave the page unable to report the outcome).
  const disconnectLocked = await ev($("simBtn") + ".disabled");
  const flashLocked = await ev($("flashBtn") + ".disabled");
  await waitFor("the deadline rollback", async () => (await fwVersion()) === startVer, 180000);
  const verAfterRollback = await fwVersion();
  check("g", "flash 0.2.0 confirmed, revert, then a fail_self_test image is refused and rolled back at the deadline",
    verNew === "0.2.0" && reconnectedG && /Update confirmed/.test(msgNew) &&
    verReverted === startVer && /sensor reported a fault/.test(msgRollback) && /was not confirmed/.test(msgRollback) &&
    verWhilePending === "0.3.0" && verAfterRollback === startVer && disconnectLocked && flashLocked,
    "start " + startVer + "; after flash " + verNew + " (" + msgNew + "); after revert " + verReverted + " (" + msgRevert.slice(0, 80) + "); rollback: " + msgRollback + " -> version " + verAfterRollback + ", reconnects " + connF + " -> " + (await connectedCount()) +
    "; while waiting: disconnect locked " + disconnectLocked + ", flash locked " + flashLocked);

  // ---- i. a sensor that is not polled yet is waited for, then the image is confirmed ----
  // slow_sensor_start answers GET_DIAGNOSTICS with "not observed" (3) for the first seconds of
  // the boot; the console must wait for real readings, not read that as healthy.
  await setValue("simNextImage", "slow_sensor_start");
  await click("simApplyBtn");
  await sleep(300);
  await flash(imgOf("0.4.0"));
  await waitFor("the slow-sensor update to be confirmed", async () => /Update confirmed/.test(await text("updateMessage")), 90000);
  await settled();
  const logI = await logText();
  const verSlow = await fwVersion();
  check("i", "an image whose sensor starts late is waited for, then confirmed (not confirmed on the startup zero)",
    verSlow === "0.4.0" && /has not produced its first readings yet/.test(logI) && /New firmware confirmed/.test(logI),
    "version " + verSlow + "; waited: " + /has not produced its first readings yet/.test(logI));

  // ---- h. no page errors, no simulator errors ----
  const errs = await ev("JSON.stringify(__errs)");
  const simBad = simOut.split(/\r?\n/).filter((l) => /AddressSanitizer|runtime error|Assertion|ASSERT|SEGV|segfault|terminate called|abort|exception/i.test(l));
  check("h", "no script errors in the page, no crash lines in the simulator output, simulator still running",
    errs === "[]" && simBad.length === 0 && simExit === null,
    "page errors " + errs + "; simulator bad lines " + JSON.stringify(simBad.slice(0, 3)) + "; exited " + JSON.stringify(simExit));
  // ---- j. a build with firmware updates off (--no-ota): no update controls, a note, the version ----
  // GET_FW_INFO answers function_not_supported, so the console hides the update controls and
  // points at the first-flash page, but still shows the version the simulator announced.
  sim.kill();
  simOut = "";
  const port2 = await freePort();
  sim = spawn(simPath, ["--fresh", "--no-ota", "--data", path.join(tmp, "simdata-no-ota"), "--port", String(port2), "--speed", speed], { stdio: ["ignore", "pipe", "pipe"] });
  sim.stdout.on("data", (d) => { simOut += d; });
  sim.stderr.on("data", (d) => { simOut += d; });
  sim.on("exit", () => { });
  await waitFor("the no-ota simulator to listen", () => /listening on/.test(simOut), 10000);
  await cdp.send("Page.navigate", { url: pathToFileURL(path.join(tmp, "page.html")).href });
  await sleep(1200);
  await setValue("simUrl", "ws://127.0.0.1:" + port2); // the wrapper preset the old port on load
  await waitFor("connect to the no-ota simulator", async () => {
    await click("simBtn");
    await sleep(150);
    return (await text("simBtn")) === "Disconnect";
  }, 10000);
  await waitFor("the update note", async () => !(await ev($("fwNoUpdates") + ".hidden")), 20000).catch(() => { });
  const controlsHidden = await ev($("fwControls") + ".hidden");
  const noteShown = !(await ev($("fwNoUpdates") + ".hidden"));
  const noteText = await text("fwNoUpdatesText");
  // The update panel's own version field stays n/a (the firmware does not put its version in the
  // discovery reply), so the running version is checked where the console shows it: the hello.
  const verOff = await text("simInfo");
  check("j", "--no-ota: update controls hidden, no-updates note shown, simulator hello still shows the version",
    controlsHidden && noteShown && noteText.length > 0 && /firmware \d+\.\d+\.\d+/.test(verOff) && /updates over USB are off/.test(simOut),
    "controls hidden " + controlsHidden + "; note shown " + noteShown + " (" + noteText.slice(0, 60) + "); version " + verOff);

  if (verbose) console.log("---- final log ----\n" + (await logText()) + "\n---- simulator output ----\n" + simOut);
}

let failed = false;
try { await main(); }
catch (e) {
  failed = true; console.log("ERROR " + e.message);
  if (verbose && cdp) {
    try {
      const expr = "(() => { const l = document.getElementById('log').innerText.split(/\\r?\\n/).slice(-40);" +
        " l.push('updateMessage: ' + document.getElementById('updateMessage').textContent); return l; })()";
      const r = await cdp.send("Runtime.evaluate", { expression: expr, returnByValue: true });
      console.log("---- page log tail ----");
      console.log(r.result.value.join(os.EOL));
    } catch (_) { }
    console.log("---- simulator output tail ----");
    console.log(simOut.split(/\r?\n/).filter((l) => !/Joystick\/E/.test(l)).slice(-40).join(os.EOL));
  }
}
finally {
  try { cdp && cdp.close(); } catch (_) { }
  try { chrome && chrome.proc.kill(); } catch (_) { }
  try { sim && sim.kill(); } catch (_) { }
  await sleep(300);
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch (_) { }
}
const bad = results.filter((r) => !r.ok);
console.log("\n" + (results.length - bad.length) + " of " + results.length + " checks passed");
process.exit(failed || bad.length ? 1 : 0);
