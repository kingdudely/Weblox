#!/usr/bin/env node
// runprobe.js — launch the web client with a patched engine module.
//
// Serves nothing itself: expects tools/serve_wasm.py on --wasm-url and
// redirects the page's WebClient.wasm request there via CDP Fetch.
//
// usage:
//   node --experimental-websocket tools/probe/runprobe.js \
//     --place 13569002876 --cookie /path/to/cookie.txt \
//     --wasm-url http://127.0.0.1:8765/WebClient.wasm \
//     --flags FFlagUseRbxTransportClient3=False --seconds 70 --out /tmp/run.jsonl
const fs = require("fs"), os = require("os"), path = require("path");
const { spawn } = require("child_process");

const UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36";
const a = (n, d) => { const i = process.argv.indexOf("--" + n); const v = process.argv[i + 1]; return i === -1 ? d : (v && !v.startsWith("--") ? v : true); };
const PLACE = a("place", "13569002876");
const SECONDS = Number(a("seconds", 70));
const OUT = a("out", "/tmp/opencode/run.jsonl");
const COOKIE = a("cookie", "/home/john/Boblox/run/cookie.txt");
const WASM_URL = a("wasm-url", "http://127.0.0.1:8765/WebClient.wasm");
const FLAGS = a("flags", "FFlagUseRbxTransportClient3=False");
const PORT = Number(a("port", 9335));

try { fs.unlinkSync(OUT); } catch (_) {}
const log = (o) => fs.appendFileSync(OUT, JSON.stringify(o) + "\n");

const flagInit = `(() => {
  const of = window.fetch;
  window.fetch = async function (u, o) {
    const r = await of(u, o);
    const s = String(u && u.url ? u.url : u);
    if (/\\/v2\\/settings\\/application\\/WebClientApp/.test(s)) {
      try {
        const j = await r.clone().json();
        j.applicationSettings = j.applicationSettings || {};
        const flags = ${JSON.stringify(FLAGS)};
        for (const kv of String(flags).split(",")) {
          const p = kv.split("="); if (p.length === 2) j.applicationSettings[p[0].trim()] = p[1].trim();
        }
        console.log("[INJ] flags applied: " + flags);
        return new Response(JSON.stringify(j), { status: 200, headers: { "content-type": "application/json" } });
      } catch (e) { console.log("[INJ] err " + e); }
    }
    return r;
  };
  console.log("[INJ] fetch hook");
})();`;

const PROFILE = fs.mkdtempSync(path.join(os.tmpdir(), "rbxrun-"));
const chrome = spawn("google-chrome", [
  "--new-window", "--remote-debugging-port=" + PORT, "--user-data-dir=" + PROFILE,
  "--no-first-run", "--no-default-browser-check", "--disable-dev-shm-usage", "--no-sandbox",
  "--autoplay-policy=no-user-gesture-required", "--user-agent=" + UA,
  "--window-size=1280,800", "about:blank",
], { stdio: ["ignore", "ignore", "ignore"] });
const kill = () => { try { chrome.kill("SIGTERM"); } catch (_) {} };
process.on("exit", kill);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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
    return new Promise((res, rej) => { pend.set(i, { res, rej }); setTimeout(() => { if (pend.delete(i)) rej(new Error("timeout " + method)); }, 30000).unref(); });
  };
  const on = (m, f) => { if (!h.has(m)) h.set(m, []); h.get(m).push(f); };

  let n = 0;
  on("Runtime.consoleAPICalled", (p) => {
    const s = (p.args || []).map((x) => x.value !== undefined ? x.value : x.description || "").join(" ");
    if (n++ < 800) log({ k: "console", s: s.slice(0, 400) });
  });
  on("Runtime.exceptionThrown", (p) => {
    const d = p.exceptionDetails || {};
    log({ k: "exc", s: String((d.exception && d.exception.description) || d.text || "").slice(0, 500) });
  });

  let wasmHits = 0;
  on("Fetch.requestPaused", async (p, sid) => {
    const u = p.request.url;
    try {
      if (/WebClient\.wasm/.test(u)) {
        wasmHits++;
        log({ k: "wasm", n: wasmHits, from: u.slice(0, 120), to: WASM_URL });
        await send("Fetch.continueRequest", { requestId: p.requestId, url: WASM_URL }, sid);
      } else {
        await send("Fetch.continueRequest", { requestId: p.requestId }, sid);
      }
    } catch (e) { log({ k: "fetcherr", e: String(e).slice(0, 200) }); }
  });

  on("Target.attachedToTarget", async (p) => {
    const sid = p.sessionId;
    log({ k: "target", type: p.targetInfo.type, url: String(p.targetInfo.url).slice(0, 120) });
    try { await send("Runtime.enable", {}, sid); } catch (_) {}
    try { await send("Fetch.enable", { patterns: [{ urlPattern: "*WebClient.wasm*" }] }, sid); } catch (_) {}
    try { await send("Runtime.runIfWaitingForDebugger", {}, sid); } catch (_) {}
  });

  const t = await send("Target.createTarget", { url: "about:blank" });
  const at = await send("Target.attachToTarget", { targetId: t.targetId, flatten: true });
  const S = at.sessionId;
  await send("Page.enable", {}, S);
  await send("Runtime.enable", {}, S);
  await send("Network.enable", {}, S);
  await send("Page.addScriptToEvaluateOnNewDocument", { source: flagInit }, S);
  await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }, S);
  await send("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false }, S);

  if (COOKIE && fs.existsSync(COOKIE)) {
    const val = fs.readFileSync(COOKIE, "utf8").trim();
    const c = await send("Network.setCookie", { name: ".ROBLOSECURITY", value: val, domain: ".roblox.com", path: "/", secure: true, httpOnly: true, sameSite: "Lax", expires: Math.floor(Date.now() / 1000) + 30 * 86400 }, S).catch(() => null);
    log({ k: "cookie", ok: !!(c && c.success) });
  }

  const url = "https://play.roblox.com/games/start?placeId=" + PLACE;
  log({ k: "nav", url });
  await send("Page.navigate", { url }, S);

  const end = Date.now() + SECONDS * 1000;
  while (Date.now() < end) await sleep(2000);

  const shot = await send("Page.captureScreenshot", { format: "png" }, S).catch(() => null);
  if (shot) fs.writeFileSync("/tmp/opencode/run.png", Buffer.from(shot.data, "base64"));
  log({ k: "done", wasmHits });
  w.close(); kill(); await sleep(300); process.exit(0);
})().catch((e) => { log({ k: "fatal", s: String(e && e.stack || e) }); kill(); process.exit(1); });
