// A browser-shaped Deno host that runs the real play.roblox.com bootstrap
// (WasmLoader -> WasmPlayer -> WebClient) from local copies of those files.
// WebGPU and WebTransport are Deno's own; only the DOM is a shape.
import { install, patchHeaders } from "../oracle/env_deno.mjs";
import { XHR, patchJoin } from "../oracle/xhr.mjs";

const DenoWorker = (globalThis as any).Worker;
console.error("[host] DenoWorker name=", (DenoWorker as any)?.name, "same=", DenoWorker === (globalThis as any).Worker);

const ORIGIN = "https://play.roblox.com";
const PAGE = ORIGIN + "/games/start";
const HASH = "6536749aa05e452da9c4d6ed4b464613ff9ba84a";
const ASSETS = `${ORIGIN}/${HASH}/assets/webclient`;
const LOCAL = "/tmp/opencode/page/";
const PLACE = Number(Deno.env.get("WC_PLACE")) || 189707;
Deno.env.set("WC_PLACE", String(PLACE));
const ENGINE = "https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient";
const JOIN_ORIGIN = "https://gamejoin.roblox.com";
const JOIN_PATHS = new Set(["/v1/join-game", "/v1/join-game-instance", "/v1/join-reserved-game", "/v1/join-private-game"]);

install("main");

globalThis.name = "main";

const localName = (u: string) => LOCAL + u.slice(u.lastIndexOf("/") + 1);

// ---- DOM bits the page itself touches --------------------------------
class HTMLScriptElement {}
Object.defineProperty(HTMLScriptElement.prototype, Symbol.toStringTag, { value: "HTMLScriptElement" });
(globalThis as any).HTMLScriptElement = HTMLScriptElement;

let currentScript: any = null;
Object.defineProperty(document, "currentScript", { configurable: true, get: () => currentScript });

async function runScript(src: string) {
  const tag = new HTMLScriptElement() as any;
  tag.src = src;
  const body = await (await fetch(src)).text();
  currentScript = tag;
  try {
    (0, eval)(body + `\n//# sourceURL=${src}`);
  } finally {
    currentScript = null;
  }
}

const realCreate = (document as any).createElement.bind(document);
(document as any).createElement = (tag: string) => (tag === "script" ? new HTMLScriptElement() : realCreate(tag));

const append = (host: any) => (c: any) => {
  host.children.push(c);
  if (c instanceof HTMLScriptElement) {
    runScript(c.src).then(() => c.onload?.()).catch((e) => { console.error("[page ] script failed", c.src, e); c.onerror?.(e); });
  }
  return c;
};
(document.body as any).appendChild = append(document.body);
(document.head as any).appendChild = append(document.head);

// ---- the real Worker: a Deno DedicatedWorkerGlobalScope --------------
(globalThis as any).Worker = class {
  _w: any;
  _ready = false;
  _q: any[] = [];
  onmessage: ((e: any) => void) | null = null;
  onerror: ((e: any) => void) | null = null;
  constructor(url: string, opts: any = {}) {
    const u = new URL("./worker.ts", import.meta.url);
    u.hash = encodeURIComponent(String(url));
    this._w = new DenoWorker(u, {
      type: "module",
      name: opts.name,
      deno: { permissions: "inherit" },
    });
    console.error("[host] worker start", u.href.slice(-60));
    this._w.onerror = (e: any) => { console.error("[host] worker error", e?.message || e); try { e?.preventDefault?.() } catch {} this.onerror?.(e); return true; };
    this._w.onmessage = (e: any) => {
      if (e.data && e.data.__ready) { console.error("[host] worker ready q=", this._q.length); this._ready = true; for (const [m, t] of this._q) { try { this._w.postMessage(m, t); console.error("[host] flushed", m && m.cmd); } catch (x: any) { console.error("[host] flush failed", x?.message || x); } } this._q.length = 0; return }
      if (e.data && e.data.__log) { console.error("[wrk]", e.data.__log); return }
      this.onmessage?.(e);
    };
  }
  postMessage(m: any, t: any) { if (!this._ready) { this._q.push([m, t]); return } this._w.postMessage(m, t); }
  addEventListener(t: string, f: any) { if (t === "message") this.onmessage = f; }
  removeEventListener() {}
  terminate() { this._w.terminate(); }
};

// ---- fetch: local files, static version, launcher patch --------------
const realFetch = globalThis.fetch;
globalThis.fetch = async (input: any, init: any = {}) => {
  const u = typeof input === "string" ? input : input.url;
  if (/^https?:\/\/(setup\.rbxcdn\.com|play\.roblox\.com)\//.test(u)) {
    try { return new Response(await Deno.readFile(localName(u))) } catch {}
  }
  if (u.startsWith("https://clientsettings.roblox.com/v2/client-version/WebClientApp")) {
    return Response.json({ version: "0.748.0.7481526", clientVersionUpload: "version-1ef59424f70d4932", bootstrapperVersion: "" });
  }
  if (u.includes("/v2/settings/application/") && Deno.env.get("WC_RENDERER") === "webgl") {
    const r = await realFetch(u, init);
    const j = await r.json();
    j.applicationSettings ||= {};
    j.applicationSettings.FFlagWasmPreferWebGPU = "False";
    return Response.json(j);
  }
  const h = new Headers(init.headers || {});
  patchHeaders(u, h);
  let body = init.body;
  let join = false;
  try { const p = new URL(u); join = String(init.method || "GET").toUpperCase() === "POST" && p.origin === JOIN_ORIGIN && JOIN_PATHS.has(p.pathname); } catch {}
  if (join && typeof body === "string") {
    const patched = patchJoin(body);
    if (patched) body = patched;
  }
  return realFetch(u, { ...init, headers: h, body });
};

// ---- boot exactly like the start page does ---------------------------
(globalThis as any).robloxWasmHostConfig = {
  server: "prod",
  useCdnBinary: true,
  workerStubUrl: `${ASSETS}/stub.js`,
};
(globalThis as any).XMLHttpRequest = XHR;

console.error("[host] page at", PAGE, "place", PLACE);
await runScript(`${ASSETS}/WasmLoader.js`);
