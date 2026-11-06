// Minimal Chrome DevTools Protocol client (Node 24 global WebSocket) and
// helpers used by sim_console.mjs. No dependencies.
import { spawn } from "node:child_process";
import net from "node:net";

export function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.once("error", reject);
    s.listen(0, "127.0.0.1", () => { const { port } = s.address(); s.close(() => resolve(port)); });
  });
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function launchChrome(chromePath, userDataDir, url, extraArgs = []) {
  const port = await freePort();
  const proc = spawn(chromePath, [
    "--headless=new", "--remote-debugging-port=" + port, "--user-data-dir=" + userDataDir,
    "--no-first-run", "--no-default-browser-check", "--disable-extensions", "--window-size=1280,1800",
    ...extraArgs, url,
  ], { stdio: "ignore" });
  let target = null;
  for (let i = 0; i < 100 && !target; i++) {
    await sleep(200);
    try {
      const list = await (await fetch("http://127.0.0.1:" + port + "/json/list")).json();
      target = list.find((t) => t.type === "page");
    } catch (_) { }
  }
  if (!target) { proc.kill(); throw new Error("Chrome did not start"); }
  return { proc, port, wsUrl: target.webSocketDebuggerUrl };
}

export async function connectCdp(wsUrl, onEvent) {
  const ws = new WebSocket(wsUrl);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = () => reject(new Error("CDP socket failed")); });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      if (m.error) reject(new Error(m.error.message)); else resolve(m.result);
    } else if (m.method && onEvent) onEvent(m);
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const i = ++id;
    pending.set(i, { resolve, reject });
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  return { send, close: () => ws.close() };
}
