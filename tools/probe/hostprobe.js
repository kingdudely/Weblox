#!/usr/bin/env node
// hostprobe.js — drive the self-hosted client (tools/host_server.py) and dump
// everything it says. No cookie injection, no wasm redirect, no flags: the host
// owns all of that.
const fs = require("fs"), os = require("os"), path = require("path");
const { spawn } = require("child_process");

const a = (n, d) => { const i = process.argv.indexOf("--" + n); const v = process.argv[i + 1]; return i === -1 ? d : (v && !v.startsWith("--") ? v : true); };
const URL_ARG = a("url", "http://127.0.0.1:8765/");
const SECONDS = Number(a("seconds", 70));
const OUT = a("out", "/tmp/opencode/host.jsonl");
const PORT = Number(a("port", 9336));
try { fs.unlinkSync(OUT); } catch (_) {}
const log = (o) => fs.appendFileSync(OUT, JSON.stringify(o) + "\n");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const PROFILE = fs.mkdtempSync(path.join(os.tmpdir(), "rbxhost-"));
const chrome = spawn("google-chrome", [
  "--new-window", "--remote-debugging-port=" + PORT, "--user-data-dir=" + PROFILE,
  "--no-first-run", "--no-default-browser-check", "--disable-dev-shm-usage", "--no-sandbox",
  "--autoplay-policy=no-user-gesture-required", "--ignore-certificate-errors",
  "--window-size=1280,800", "about:blank",
], { stdio: ["ignore", "ignore", "ignore"] });
const kill = () => { try { chrome.kill("SIGTERM"); } catch (_) {} };
process.on("exit", kill);

async function wsUrl() {
  for (let i = 0; i < 120; i++) {
    try { const r = await fetch("http://127.0.0.1:" + PORT + "/json/version"); if (r.ok) return (await r.json()).webSocketDebuggerUrl; } catch (_) {}
    await sleep(100);
  }
  throw new Error("no cdp");
}

(async () => {
  const w = new WebSocket(await wsUrl());
  await new Promise((res, rej) => { w.addEventListener("open", res, { once: true }); w.addEventListener("error", rej, { once: true }); });
  let id = 0; const pend = new Map(); const h = new Map();
  w.addEventListener("message", (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pend.has(m.id)) { const p = pend.get(m.id); pend.delete(m.id); m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result); }
    else if (m.method) (h.get(m.method) || []).forEach((f) => f(m.params, m.sessionId));
  });
  const send = (method, params, sid) => {
    const i = ++id; const p = { id: i, method, params: params || {} }; if (sid) p.sessionId = sid;
    w.send(JSON.stringify(p));
    return new Promise((res, rej) => { pend.set(i, { res, rej }); setTimeout(() => { if (pend.delete(i)) rej(new Error("timeout " + method)); }, 60000); });
  };
  const on = (m, f) => { if (!h.has(m)) h.set(m, []); h.get(m).push(f); };

  let n = 0;
  on("Runtime.consoleAPICalled", (p) => {
    const s = (p.args || []).map((x) => x.value !== undefined ? x.value : x.description || "").join(" ");
    if (n++ < 3000) log({ k: "console", s: s.slice(0, 500) });
  });
  on("Runtime.exceptionThrown", (p) => {
    const d = p.exceptionDetails || {};
    log({ k: "exc", s: String((d.exception && d.exception.description) || d.text || "").slice(0, 400) });
  });
  on("Network.loadingFailed", (p) => log({ k: "netfail", e: p.errorText, blocked: p.blockedReason }));
  on("Network.responseReceived", (p) => {
    const u = p.response.url;
    if (/\/proxy\/|join-game|join\.ashx/.test(u)) log({ k: "res", u: u.slice(0, 130), st: p.response.status });
  });
  on("Target.attachedToTarget", async (p) => {
    const sid = p.sessionId;
    try { await send("Runtime.enable", {}, sid); } catch (_) {}
    try { await send("Network.enable", {}, sid); } catch (_) {}
    try { await send("Runtime.runIfWaitingForDebugger", {}, sid); } catch (_) {}
  });

  const t = await send("Target.createTarget", { url: "about:blank" });
  const at = await send("Target.attachToTarget", { targetId: t.targetId, flatten: true });
  const S = at.sessionId;
  await send("Page.enable", {}, S);
  await send("Runtime.enable", {}, S);
  await send("Network.enable", {}, S);
  await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }, S);
  await send("Page.navigate", { url: URL_ARG }, S);
  log({ k: "nav", url: URL_ARG });

  const end = Date.now() + SECONDS * 1000;
  while (Date.now() < end) await sleep(2000);
  const dbg = await send("Runtime.evaluate", { expression: "JSON.stringify({rw: (window.__rbxRewrites||0), xhr: String(XMLHttpRequest.prototype.open).slice(0,60), cis: self.crossOriginIsolated})", returnByValue: true }, S).catch(() => null);
  if (dbg && dbg.result) log({ k: "dbg", v: dbg.result.value });
  const shot = await send("Page.captureScreenshot", { format: "png" }, S).catch(() => null);
  if (shot) fs.writeFileSync("/tmp/opencode/host.png", Buffer.from(shot.data, "base64"));
  log({ k: "done" });
  w.close(); kill(); await sleep(300); process.exit(0);
})().catch((e) => { log({ k: "fatal", s: String(e && e.stack || e) }); kill(); process.exit(1); });
