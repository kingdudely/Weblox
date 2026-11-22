// Browser surface for running the stock glue off Chromium.  Only what the
// join path touches is real; rendering is a shape, not a renderer.
import { readFileSync } from "node:fs";
import { XHR } from "./xhr.mjs";
import { makeGL, make2D } from "./gl.mjs";
import { makeOPFS } from "./opfs.mjs";
import * as wt from "node:worker_threads";

const await_worker_threads = wt;
export const GLUE = "/tmp/opencode/oracle/WebClient.traced.js";

// Deno's own navigator.gpu / WebTransport are the same wgpu and quinn the native
// host uses; keep them instead of stubbing.
const REAL_GPU = globalThis.navigator?.gpu;

export const UA =
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36";
export const ORIGIN = "https://play.roblox.com";
export const PAGE = ORIGIN + "/games/start";
const COOKIE = readFileSync("/home/john/king.txt", "utf8").trim()
  .replace(/^\.ROBLOSECURITY=/, "");

const log = (...a) => console.error("[env ]", ...a);

class El {
  constructor(tag) {
    this.tagName = String(tag).toUpperCase();
    this.style = {};
    this.dataset = {};
    this.children = [];
    this.hidden = false;
    this.parentElement = null;
    this.listeners = new Map();
    this.classList = {
      _s: new Set(),
      add: (...c) => c.forEach((x) => this.classList._s.add(x)),
      remove: (...c) => c.forEach((x) => this.classList._s.delete(x)),
      toggle: (c, f) => (f ?? !this.classList._s.has(c)) ? this.classList._s.add(c) : this.classList._s.delete(c),
      contains: (c) => this.classList._s.has(c),
    };
  }
  addEventListener(t, f) { let a = this.listeners.get(t); if (!a) this.listeners.set(t, a = []); a.push(f); }
  removeEventListener(t, f) { const a = this.listeners.get(t); if (a) this.listeners.set(t, a.filter((x) => x !== f)); }
  dispatch(t, e = {}) { for (const f of this.listeners.get(t) || []) f({ type: t, target: this, ...e }); }
  appendChild(c) { this.children.push(c); c.parentElement = this; return c; }
  append(...c) { for (const x of c) this.appendChild(x); }
  prepend(c) { this.children.unshift(c); c.parentElement = this; }
  insertBefore(c) { return this.appendChild(c); }
  removeChild(c) { this.children = this.children.filter((x) => x !== c); }
  remove() { this.parentElement?.removeChild(this); }
  setAttribute(k, v) { this[k] = String(v); }
  getAttribute(k) { return this[k] ?? null; }
  removeAttribute(k) { delete this[k]; }
  hasAttribute(k) { return k in this; }
  focus() {}
  blur() {}
  click() {}
  querySelector() { return null; }
  querySelectorAll() { return []; }
  closest() { return null; }
  getBoundingClientRect() { return { x: 0, y: 0, left: 0, top: 0, right: 1280, bottom: 720, width: 1280, height: 720 }; }
  get clientWidth() { return 1280; }
  get clientHeight() { return 720; }
  get clientTop() { return 0; }
  get clientLeft() { return 0; }
  get offsetWidth() { return 1280; }
  get offsetHeight() { return 720; }
  get scrollWidth() { return 1280; }
  get scrollHeight() { return 720; }
  getContextSafariWebGL2Fixed() { return null; }
}

class Canvas extends El {
  constructor() {
    super("canvas");
    this.id = "canvas";
    this.oc = new OffscreenCanvas(1280, 720);
    this.gl = null;
    this.gpu = null;
  }
  get width() { return this.oc.width; }
  set width(v) { this.oc.width = v; }
  get height() { return this.oc.height; }
  set height(v) { this.oc.height = v; }
  getContext(kind, attrs) {
    if (kind === "webgpu") return (this.gpu ||= this.oc.getContext("webgpu"));
    if (kind === "webgl2") return (this.gl ||= makeGL(this));
    if (kind === "webgl") return null; // the glue only asks for webgl2
    if (kind === "2d") return make2D(this);
    return null;
  }
  toDataURL() { return "data:image/png;base64,"; }
  toBlob(cb) { cb(new Blob()); }
}

export function install(kind) {
  const worker = kind === "worker";
  const listeners = new Map();
  const add = (t, f) => { let a = listeners.get(t); if (!a) listeners.set(t, a = []); a.push(f); };
  const remove = (t, f) => { const a = listeners.get(t); if (a) listeners.set(t, a.filter((x) => x !== f)); };
  const fire = (t, e) => { for (const f of listeners.get(t) || []) f({ type: t, ...e }); };

  if (!worker) globalThis.window = globalThis;
  globalThis.self = globalThis;
  // the glue gates its JSPI (fiber) machinery on this interface existing
  if (!globalThis.WorkerGlobalScope) globalThis.WorkerGlobalScope = class WorkerGlobalScope {};
  // Deno registers its own message pump through the *global* addEventListener;
  // shadowing it without forwarding silently kills every worker message.
  const priorAdd = globalThis.addEventListener?.bind(globalThis);
  const priorRemove = globalThis.removeEventListener?.bind(globalThis);
  globalThis.addEventListener = (t, f, o) => { priorAdd?.(t, f, o); add(t, f); };
  globalThis.removeEventListener = (t, f, o) => { priorRemove?.(t, f, o); remove(t, f); };
  globalThis.dispatchEvent = (e) => { fire(e.type, e); return true; };
  globalThis.HTMLElement = El;
  globalThis.HTMLCanvasElement = Canvas;
  globalThis.HTMLVideoElement = class extends El {};
  globalThis.HTMLAudioElement = class extends El {};
  globalThis.WebGLRenderingContext = class {};
  globalThis.Image = class extends El { set src(v) { this._src = v; } get src() { return this._src || ""; } };
  if (!process.env.WC_NOAUD) globalThis.AudioContext = class {
    constructor() {
      this.sampleRate = 44100;
      this.currentTime = 0;
      this.state = "running";
      this.destination = { channelCount: 2, maxChannelCount: 2, connect() {}, disconnect() {} };
      this.audioWorklet = { addModule: async () => {} };
    }
    resume() { console.error("[aud] resume"); this.state = "running"; return Promise.resolve(); }
    suspend() { console.error("[aud] suspend"); this.state = "suspended"; return Promise.resolve(); }
    close() { return Promise.resolve(); }
    createGain() { return { gain: { value: 1 }, connect() {}, disconnect() {} }; }
    createBufferSource() { return { buffer: null, connect() {}, disconnect() {}, start() {}, stop() {} }; }
    createScriptProcessor(a, b, c) {
      const size = [a, b, c].find((x) => x >= 256) || 4096;
      console.error("[aud] createScriptProcessor", a, b, c, "->", size);
      const n = { bufferSize: size, onaudioprocess: null,
        connect() { console.error("[aud] connect"); }, disconnect() {} };
      const buf = () => ({ numberOfChannels: 2, length: size, sampleRate: 44100,
        getChannelData: () => new Float32Array(size) });
      let fired = 0;
      setInterval(() => { if (n.onaudioprocess && fired++ < 3) console.error("[aud] fire"); if (n.onaudioprocess) n.onaudioprocess({ outputBuffer: buf(), inputBuffer: buf() }); }, Math.max(5, (size / 48) | 0));
      return n;
    }
  };
  globalThis.webkitAudioContext = globalThis.AudioContext;

  globalThis.devicePixelRatio = 1;
  globalThis.innerWidth = 1280;
  globalThis.innerHeight = 720;
  globalThis.outerWidth = 1280;
  globalThis.outerHeight = 720;
  globalThis.screen = { width: 1280, height: 720, availWidth: 1280, availHeight: 720, colorDepth: 24, pixelDepth: 24 };
  globalThis.scrollX = 0;
  globalThis.scrollY = 0;
  globalThis.pageXOffset = 0;
  globalThis.pageYOffset = 0;
  globalThis.crossOriginIsolated = true;
  Object.defineProperty(globalThis, "location", { configurable: true, value: {
    href: PAGE, origin: ORIGIN, protocol: "https:", host: "play.roblox.com",
    hostname: "play.roblox.com", port: "", pathname: "/games/start",
    search: worker ? "" : "?placeId=" + (process.env.WC_PLACE || "189707") +
      (process.env.WC_RENDERER ? "&renderer=" + process.env.WC_RENDERER : ""), hash: "",
    ancestorOrigins: [], reload() {}, assign() {}, replace() {}, toString() { return this.href; },
  }});
  globalThis.addEventListener("error", (e) => log("window error", e.message || e));
  globalThis.onerror = null;
  globalThis.open = () => null;
  globalThis.close = () => {};
  globalThis.focus = () => {};
  globalThis.alert = () => {};
  globalThis.getComputedStyle = () => ({ getPropertyValue: () => "" });
  globalThis.matchMedia = (q) => ({
    media: q,
    matches: q.includes("pointer: fine") || q.includes("prefers-color-scheme: light"),
    addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {},
  });
  globalThis.requestAnimationFrame = (f) => setTimeout(() => f(performance.now()), 16);
  globalThis.cancelAnimationFrame = (i) => clearTimeout(i);
  globalThis.ResizeObserver = class { observe() {} unobserve() {} disconnect() {} };

  const store = new Map();
  globalThis.localStorage = {
    getItem: (k) => store.get(k) ?? null,
    setItem: (k, v) => store.set(k, String(v)),
    removeItem: (k) => store.delete(k),
    clear: () => store.clear(),
    key: (i) => [...store.keys()][i] ?? null,
    get length() { return store.size; },
  };

  const nav = {
    userAgent: UA,
    appVersion: UA.replace("Mozilla/", ""),
    appName: "Netscape",
    platform: "Linux x86_64",
    vendor: "Google Inc.",
    product: "Gecko",
    language: "en-US",
    languages: ["en-US", "en"],
    hardwareConcurrency: 8,
    deviceMemory: 8,
    maxTouchPoints: 0,
    onLine: true,
    cookieEnabled: true,
    webdriver: false,
    gpu: REAL_GPU,
    doNotTrack: null,
    userAgentData: { mobile: false, platform: "Linux x86_64", brands: [{ brand: "Chromium", version: "157" }] },
    storage: {
      getDirectory: async () => opfs,
      estimate: async () => ({ usage: 0, quota: 1 << 30 }),
      persist: async () => true,
      persisted: async () => true,
    },
    locks: {
      request: async (name, opts, cb) => {
        if (typeof opts === "function") { cb = opts; }
        return cb({ name, mode: "exclusive" });
      },
    },
    keyboard: { getLayoutMap: async () => new Map() },
    clipboard: { writeText: async () => {}, readText: async () => "" },
    share: async () => {},
    permissions: { query: async () => ({ state: "granted" }) },
  };
  Object.defineProperty(globalThis, "navigator", { value: nav, configurable: true });

  const opfs = makeOPFS();

  let cookieSet = ".ROBLOSECURITY=" + COOKIE + "; RBXEventTrackerV2=browserid=oracle";
  const document = {
    currentScript: { src: "file:///tmp/opencode/oracle/WebClient.traced.js" },
    documentElement: new El("html"),
    head: new El("head"),
    body: new El("body"),
    title: "Roblox",
    readyState: "complete",
    visibilityState: "visible",
    hidden: false,
    fullscreenElement: null,
    addEventListener: add,
    removeEventListener: remove,
    createElement: (tag) => (tag === "canvas" ? new Canvas() : new El(tag)),
    createElementNS: (_ns, tag) => (tag === "canvas" ? new Canvas() : new El(tag)),
    getElementById: (id) => document._ids.get(id) || null,
    querySelector: (s) => document._ids.get(s.replace(/^#/, "")) || null,
    querySelectorAll: () => [],
    getElementsByTagName: () => [],
    _ids: new Map(),
    hasFocus: () => true,
    exitFullscreen: async () => {},
  };
  const canvas = new Canvas();
  document.body.appendChild(canvas);
  for (const id of ["canvas", "status", "server", "proxyenabled", "loading-overlay",
    "input-container", "input-textarea", "input-input"]) {
    document._ids.set(id, id === "canvas" ? canvas : new El("div"));
  }
  Object.defineProperty(document, "cookie", { get: () => cookieSet, set: (v) => { cookieSet = v; } });
  globalThis.document = document;
  globalThis.canvasEl = canvas;

  const realFetch = globalThis.fetch;
  globalThis.fetch = async (input, init = {}) => {
    const url = typeof input === "string" ? input : input.url;
    log("fetch", url);
    const h = new Headers(init.headers || (typeof input === "object" ? input.headers : undefined));
    patchHeaders(url, h);
    return realFetch(url, { ...init, headers: h });
  };

  globalThis.XMLHttpRequest = XHR;
  const { Worker: NodeWorker } = await_worker_threads;
  globalThis.Worker = class {
    constructor(_url, opts = {}) {
      this._w = new NodeWorker(new URL("./worker-run.deno.mjs", import.meta.url), {
        workerData: { name: opts.name || "worker", js: GLUE },
      });
      this._w.on("message", (data) => this.onmessage?.({ data }));
      this._w.on("error", (e) => this.onerror?.({ message: e.message, filename: "worker", lineno: 0 }));
      this._w.unref();
    }
    postMessage(m, t) { this._w.postMessage(m, t); }
    terminate() { this._w.terminate(); }
    addEventListener(t, f) { if (t === "message") this.onmessage = f; }
  };

  const WS = globalThis.WebSocket;
  if (WS) globalThis.WebSocket = class extends WS {
    constructor(url, protocols) { log("ws", url); super(url, protocols); }
  };

  if (typeof WebAssembly.Suspending === "undefined") throw new Error("no JSPI");
  {
    const S = WebAssembly.Suspending, P = WebAssembly.promising;
    Object.defineProperty(WebAssembly, "Suspending", { configurable: true, value: function (fn) {
      return new S(function (...a) {
        try { return fn(...a) } catch (e) {
          if (e instanceof WebAssembly.Exception) console.error("[sus ] threw in import", fn.name || "?");
          throw e;
        }
      });
    }});
    Object.defineProperty(WebAssembly, "promising", { configurable: true, value: function (fn) {
      const g = P(fn);
      return function (...a) {
        try { return g(...a) } catch (e) {
          if (e instanceof WebAssembly.Exception) console.error("[prm ] threw in export", fn.name || "?");
          throw e;
        }
      };
    }});
  }
}

export function patchHeaders(url, h) {
  let host = "";
  try { host = new URL(url).hostname; } catch { return; }
  if (!/(^|\.)(roblox\.com|rbxcdn\.com|robloxlabs\.com)$/.test(host)) return;
  if (!h.has("cookie")) h.set("cookie", ".ROBLOSECURITY=" + COOKIE + "; RBXEventTrackerV2=browserid=oracle");
  if (!h.has("user-agent")) h.set("user-agent", UA);
  h.set("origin", ORIGIN);
  h.set("referer", PAGE + location.search);
  h.set("rbx-user-agent", "Roblox/Darwin");
}

export { COOKIE, log };
