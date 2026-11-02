#!/usr/bin/env node
const fs = require("fs"), os = require("os"), path = require("path");
const { spawn } = require("child_process");
const UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36";
const a = (n, d) => { const i = process.argv.indexOf("--" + n); const v = process.argv[i + 1]; return i === -1 ? d : (v && !v.startsWith("--") ? v : true); };
const URL_ARG = a("url", "about:blank"), SECONDS = Number(a("seconds", 75));
const OUT = a("out", "/tmp/opencode/tap.jsonl"), COOKIE = a("cookie", "/home/john/Boblox/run/cookie.txt");
const HOOK = fs.readFileSync(a("hook", "/tmp/opencode/tap.js"), "utf8");
const PORT = a("port", 9334);
try { fs.unlinkSync(OUT); } catch (_) {}
const PROFILE = fs.mkdtempSync(path.join(os.tmpdir(), "cdpt-"));
const chrome = spawn("google-chrome", ["--new-window", "--remote-debugging-port=" + PORT, "--user-data-dir=" + PROFILE,
  "--no-first-run", "--no-default-browser-check", "--disable-dev-shm-usage", "--no-sandbox",
  "--autoplay-policy=no-user-gesture-required", "--ignore-certificate-errors",
  "--disable-features=DisableLoadExtensionCommandLineSwitch",
  "--load-extension=" + (a("ext", "") || ""), "--disable-extensions-except=" + (a("ext", "") || ""), "--user-agent=" + UA, "--window-size=1280,800", "about:blank"],
  { stdio: ["ignore", "ignore", "ignore"] });
const kill = () => { try { chrome.kill("SIGTERM"); } catch (_) {} };
process.on("exit", kill);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (o) => fs.appendFileSync(OUT, JSON.stringify(o) + "\n");
async function wsUrl() { for (let i = 0; i < 120; i++) { try { const r = await fetch("http://127.0.0.1:" + PORT + "/json/version"); if (r.ok) return (await r.json()).webSocketDebuggerUrl; } catch (_) {} await sleep(100); } throw new Error("no cdp"); }
(async () => {
  const w = new WebSocket(await wsUrl());
  await new Promise((res, rej) => { w.addEventListener("open", res, { once: true }); w.addEventListener("error", rej, { once: true }); });
  let id = 0; const pend = new Map(); const h = new Map();
  w.addEventListener("message", (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pend.has(m.id)) { const p = pend.get(m.id); pend.delete(m.id); m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result); }
    else if (m.method) (h.get(m.method) || []).forEach((f) => f(m.params, m.sessionId));
  });
  const send = (method, params, sid) => { const i = ++id; const p = { id: i, method, params: params || {} }; if (sid) p.sessionId = sid; w.send(JSON.stringify(p));
    return new Promise((res, rej) => { pend.set(i, { res, rej }); setTimeout(() => { if (pend.delete(i)) rej(new Error("timeout " + method)); }, 20000).unref(); }); };
  const on = (m, f) => { if (!h.has(m)) h.set(m, []); h.get(m).push(f); };
  on("Runtime.consoleAPICalled", (p) => {
    const s = (p.args || []).map((x) => x.value !== undefined ? x.value : x.description || "").join(" ");
    if (globalThis.__n === undefined) globalThis.__n = 0;
    if (globalThis.__n++ < 600) log({ k: "console", s: s.slice(0, 400) });
  });
  on("Target.attachedToTarget", async (p) => {
    const sid = p.sessionId, t = p.targetInfo;
    log({ k: "target", type: t.type, url: String(t.url).slice(0, 160) });
    try { await send("Runtime.enable", {}, sid); } catch (_) {}
    const isWorker = /worker/i.test(t.type);
    if (isWorker) {
      try { await send("Runtime.evaluate", { expression: HOOK, awaitPromise: false }, sid); } catch (e) { log({ k: "hookerr", e: String(e).slice(0, 300) }); }
      log({ k: "hooked", type: t.type, url: String(t.url).slice(0, 160) });
    }
    try { await send("Runtime.runIfWaitingForDebugger", {}, sid); } catch (_) {}
  });
  const t = await send("Target.createTarget", { url: "about:blank" });
  const at = await send("Target.attachToTarget", { targetId: t.targetId, flatten: true });
  const S = at.sessionId;
  await send("Page.enable", {}, S); await send("Runtime.enable", {}, S);
  await send("Network.enable", {}, S);
  await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }, S);
  await send("Page.addScriptToEvaluateOnNewDocument", { source: HOOK }, S).catch((e) => log({ k: "initerscript", e: String(e).slice(0,200) }));
  await send("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false }, S);
  const val = fs.readFileSync(COOKIE, "utf8").trim();
  try { const c = await send("Network.setCookie", { name: ".ROBLOSECURITY", value: val, domain: ".roblox.com", path: "/", secure: true, httpOnly: true, sameSite: "Lax", expires: Math.floor(Date.now() / 1000) + 30 * 86400 }, S); log({ k: "cookie", ok: !!(c && c.success) }); } catch (e) { log({ k: "cookie", ok: false }); }
  log({ k: "nav", url: URL_ARG });
  await send("Page.navigate", { url: URL_ARG }, S);
  const end = Date.now() + SECONDS * 1000;
  while (Date.now() < end) { await sleep(3000); const st = await send("Runtime.evaluate", { expression: "JSON.stringify({t:document.title,c:document.querySelectorAll('canvas').length})", returnByValue: true }, S).catch(() => null); if (st && st.result && st.result.value) log({ k: "page", ...JSON.parse(st.result.value) }); }
  const shot = await send("Page.captureScreenshot", { format: "png" }, S).catch(() => null);
  if (shot) fs.writeFileSync("/tmp/opencode/tap.png", Buffer.from(shot.data, "base64"));
  w.close(); kill(); await sleep(300); process.exit(0);
})().catch((e) => { log({ k: "fatal", s: String(e && e.stack || e) }); kill(); process.exit(1); });
