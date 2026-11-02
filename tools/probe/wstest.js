#!/usr/bin/env node
/* wstest.js — can a page reach a loopback socket?
 *
 * Diagnostic for the page <-> udp_bridge link. The engine page is https, the
 * bridge is plain ws:// on 127.0.0.1, so this checks each layer separately:
 * fetch, WebSocket, and a top-level navigation, from about:blank and from
 * https://play.roblox.com.
 *
 * usage: node --experimental-websocket tools/probe/wstest.js [--chrome-arg ...]
 */

const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const args = process.argv.slice(2);
const PORT = 9337;
const TARGET = "http://127.0.0.1:37999/";
const WSURL = "ws://127.0.0.1:37999/";

const extra = [];
for (let i = 0; i < args.length; i++) if (args[i] === "--chrome-arg") extra.push(args[++i]);

const PROFILE = fs.mkdtempSync(path.join(os.tmpdir(), "cdpws-"));
const chrome = spawn(
  "google-chrome",
  [
    "--new-window",
    "--remote-debugging-port=" + PORT,
    "--user-data-dir=" + PROFILE,
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-dev-shm-usage",
    "--no-sandbox",
    ...extra,
    "about:blank",
  ],
  { stdio: ["ignore", "ignore", "ignore"] }
);
const kill = () => { try { chrome.kill("SIGKILL"); } catch (_) {} };
process.on("exit", kill);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  let ws;
  for (let i = 0; i < 60; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/json/version`);
      await r.json();
      ws = new WebSocket((await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json()).webSocketDebuggerUrl);
      break;
    } catch (_) {
      await sleep(250);
    }
  }
  if (!ws) throw new Error("no devtools");
  await new Promise((r) => (ws.onopen = r));

  let id = 0;
  const pending = new Map();
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      pending.get(m.id)(m);
      pending.delete(m.id);
    }
  };
  const send = (method, params = {}, sessionId) =>
    new Promise((res) => {
      const n = ++id;
      pending.set(n, res);
      ws.send(JSON.stringify({ id: n, method, params, sessionId }));
    });

  const t = await send("Target.createTarget", { url: "about:blank" });
  const att = await send("Target.attachToTarget", { targetId: t.result.targetId, flatten: true });
  const S = att.result.sessionId;
  await send("Page.enable", {}, S);
  await send("Runtime.enable", {}, S);

  const probe = `(async () => {
    const out = { origin: location.origin };
    const t0 = performance.now();
    try {
      const r = await fetch(${JSON.stringify(TARGET)});
      out.fetch = "status " + r.status;
    } catch (e) { out.fetch = "err " + String(e); }
    out.fetchMs = Math.round(performance.now() - t0);
    out.ws = await new Promise((res) => {
      const w = new WebSocket(${JSON.stringify(WSURL)});
      const t1 = performance.now();
      const done = (s) => res(s + " after " + Math.round(performance.now() - t1) + "ms");
      w.onopen = () => done("open");
      w.onerror = () => done("error");
      w.onclose = (e) => done("close " + e.code);
      setTimeout(() => done("timeout state=" + w.readyState), 4000);
    });
    return out;
  })()`;

  for (const url of ["about:blank", "https://play.roblox.com/"]) {
    await send("Page.navigate", { url }, S);
    await sleep(url === "about:blank" ? 500 : 6000);
    const r = await send(
      "Runtime.evaluate",
      { expression: probe, awaitPromise: true, returnByValue: true },
      S
    );
    console.log(url, "->", JSON.stringify(r.result && r.result.result && r.result.result.value));
  }
  kill();
  process.exit(0);
}

main().catch((e) => {
  console.error(String(e));
  kill();
  process.exit(1);
});
