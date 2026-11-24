// The browser surface the stock WebClient glue expects, laid over Deno.
//
// The glue is written against a browser: a canvas, a document, window events,
// a Worker per pthread, fetch/XHR, localStorage, OPFS, AudioContext.  Every
// one of those exists in Deno or is a few lines of shape; what matters is that
// they behave like the browser's (see `addEventListener` below) instead of
// being reimplemented in Rust.
//
// `install("main")` on the desktop app is handed a `Host`: the window's own
// WebGPU surface as the canvas, and the window's input events as the page's
// events, so the engine renders and listens to a real window.
import { XHR } from "./xhr.ts";
import { makeGL, make2D } from "./gl.ts";
import { makeOPFS } from "./opfs.ts";

export const UA =
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36";
export const ORIGIN = "https://play.roblox.com";
export const PAGE = ORIGIN + "/games/start";

export const log = (...a: any[]) => console.error("[env ]", ...a);

/** The native things a real window adds. All optional: headless has none. */
export interface Host {
  /** The window's WebGPU surface; the canvas presents into it. */
  surface: any;
  width(): number;
  height(): number;
  dpr(): number;
  /** Subscribe to a real window event; returns an unsubscribe. */
  on(type: string, fn: (e: any) => void): () => void;
  setTitle(title: string): void;
  /** Called when the engine presents a frame. */
  frame?(): void;
  /** Called when the GPU device behind the surface is lost. */
  deviceLost?(info: any): void;
  /** Called at engine milestones so the watchdog can adapt its deadline. */
  progress?(milestone: string): void;
  /** Called when WebTransport never yields server data: the server is dead. */
  handshakeFail?(reason: string): void;
}

/** Build a synthetic event the way the DOM would expose it. */
export function ev(type: string, props: Record<string, any> = {}) {
  const e = new Event(type);
  for (const [k, v] of Object.entries(props)) Object.defineProperty(e, k, { value: v, configurable: true, enumerable: true });
  return e;
}
export const send = (target: EventTarget, type: string, props: Record<string, any> = {}) => target.dispatchEvent(ev(type, props));

// Deno's desktop events deliberately mirror only part of the DOM event.
// Emscripten reads the rest (keyCode, buttons, movementX...), so fill it in.
const VK: Record<string, number> = {
  Backspace: 8, Tab: 9, Enter: 13, Shift: 16, Control: 17, Alt: 18, Pause: 19, CapsLock: 20,
  Escape: 27, " ": 32, PageUp: 33, PageDown: 34, End: 35, Home: 36,
  ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Insert: 45, Delete: 46,
  ContextMenu: 93, NumLock: 144, ScrollLock: 145,
  ";": 186, "=": 187, ",": 188, "-": 189, ".": 190, "/": 191, "`": 192,
  "[": 219, "\\": 220, "]": 221, "'": 222,
};
export function keyCodeOf(e: any): number {
  if (Number.isInteger(e.keyCode)) return e.keyCode;
  const c = e.code || "";
  if (/^Key[A-Z]$/.test(c)) return c.charCodeAt(3);
  if (/^Digit[0-9]$/.test(c)) return c.charCodeAt(5);
  if (/^Numpad[0-9]$/.test(c)) return c.charCodeAt(6) + 96;
  if (/^F([1-9]|1[0-2])$/.test(c)) return 111 + Number(c.slice(1));
  return VK[c] ?? VK[e.key] ?? (e.key?.length === 1 ? e.key.toUpperCase().charCodeAt(0) : 0);
}

/** An event cannot be dispatched twice while a dispatch is in flight, and the
 *  engine needs fields Deno omits. Rebuild it with the full browser shape. */
export function cloneEvent(e: Event, extra: Record<string, any> = {}): Event {
  const raw: any = e;
  const C = raw.constructor as any;
  let out: any;
  try { out = new C(e.type, e); } catch { out = new Event(e.type); }
  const fields: Record<string, any> = {
    keyCode: keyCodeOf(raw),
    which: raw.type.startsWith("key") ? keyCodeOf(raw) : (raw.button ?? 0) + 1,
    charCode: raw.type.startsWith("key") && raw.key?.length === 1 ? raw.key.charCodeAt(0) : 0,
    movementX: 0, movementY: 0,
    buttons: raw.type.startsWith("mouse") || raw.type.startsWith("pointer") ? 1 : 0,
    offsetX: raw.clientX ?? 0, offsetY: raw.clientY ?? 0,
    ...extra,
  };
  for (const [k, v] of Object.entries(fields)) Object.defineProperty(out, k, { value: v, configurable: true, enumerable: true });
  return out;
}

const traceInput = () => Deno.env.get("WC_INPUT") === "1";

export interface SurfaceContext {
  canvas: Canvas;
  configure(cfg: Record<string, unknown>): void;
  unconfigure(): void;
  getCurrentTexture(): any;
}

export class El extends EventTarget {
  tagName: string;
  style: Record<string, any> = {};
  dataset: Record<string, any> = {};
  children: any[] = [];
  hidden = false;
  parentElement: any = null;
  id = "";
  private _w = 1280;
  private _h = 720;
  classList: any;

  constructor(tag: string) {
    super();
    this.tagName = String(tag).toUpperCase();
    const s = new Set<string>();
    this.classList = {
      add: (...c: string[]) => c.forEach((x) => s.add(x)),
      remove: (...c: string[]) => c.forEach((x) => s.delete(x)),
      toggle: (c: string, f?: boolean) => ((f ?? !s.has(c)) ? s.add(c) : s.delete(c)),
      contains: (c: string) => s.has(c),
    };
  }
  /** Deliver an already-built event to this element's handlers. */
  emit(e: Event) { this.dispatchEvent(e); }
  appendChild(c: any) { this.children.push(c); c.parentElement = this; return c; }
  append(...c: any[]) { for (const x of c) this.appendChild(x); }
  prepend(c: any) { this.children.unshift(c); c.parentElement = this; }
  insertBefore(c: any) { return this.appendChild(c); }
  removeChild(c: any) { this.children = this.children.filter((x) => x !== c); }
  remove() { this.parentElement?.removeChild(this); }
  setAttribute(k: string, v: any) { (this as any)[k] = String(v); }
  getAttribute(k: string) { return (this as any)[k] ?? null; }
  removeAttribute(k: string) { delete (this as any)[k]; }
  hasAttribute(k: string) { return k in this; }
  focus() {}
  blur() {}
  click() { send(this, "click"); }
  querySelector() { return null; }
  querySelectorAll() { return []; }
  closest() { return null; }
  getBoundingClientRect() {
    return { x: 0, y: 0, left: 0, top: 0, right: this.clientWidth, bottom: this.clientHeight, width: this.clientWidth, height: this.clientHeight };
  }
  get width() { return this._w; }
  set width(v: number) { this._w = v; }
  get height() { return this._h; }
  set height(v: number) { this._h = v; }
  get clientWidth() { return this._w; }
  get clientHeight() { return this._h; }
  get clientTop() { return 0; }
  get clientLeft() { return 0; }
  get offsetWidth() { return this.clientWidth; }
  get offsetHeight() { return this.clientHeight; }
  get scrollWidth() { return this.clientWidth; }
  get scrollHeight() { return this.clientHeight; }
}

/**
 * The presentation canvas. With a `Host` it *is* the window surface: sizing it
 * sizes the surface, and every frame is presented when the engine's
 * `presentBrowserFrame` has submitted its copy.
 */
export class Canvas extends El {
  host?: Host;
  private bw = 0;
  private bh = 0;
  private ctx?: SurfaceContext;
  private raw?: any;
  private cfg?: Record<string, unknown>;
  private gl?: any;
  private oc?: OffscreenCanvas;
  private gpu?: any;

  constructor(host?: Host) {
    super("canvas");
    this.id = "canvas";
    this.host = host;
    if (host) {
      // The backing store starts at the window's content size; the surface
      // follows `width`/`height`, while `clientWidth` stays the CSS size.
      this.bw = host.width();
      this.bh = host.height();
      this.resize(this.bw, this.bh);
    } else {
      this.oc = new OffscreenCanvas(1280, 720);
    }
  }
  override get width() { return this.host ? this.bw : this.oc!.width; }
  override set width(v: number) { if (this.host) this.resize(v, this.bh); else this.oc!.width = v; }
  override get height() { return this.host ? this.bh : this.oc!.height; }
  override set height(v: number) { if (this.host) this.resize(this.bw, v); else this.oc!.height = v; }
  override get clientWidth() { return this.host ? this.host.width() : 1280; }
  override get clientHeight() { return this.host ? this.host.height() : 720; }

  /** Resize the window surface and re-configure it for the new size. */
  resize(w: number, h: number) {
    if (!w || !h) return;
    this.bw = w;
    this.bh = h;
    const s = this.raw;
    if (!s || (s.width === w && s.height === h)) return;
    s.width = w;
    s.height = h;
    if (this.cfg) s.configure(this.cfg);
  }

  getContext(kind: string, attrs?: any) {
    if (kind === "webgpu") {
      if (this.ctx) return this.ctx;
      if (!this.host) return (this.gpu ||= this.oc!.getContext("webgpu"));
      const surface = this.host.surface;
      const raw = (this.raw = surface.getContext("webgpu"));
      const watched = new WeakSet<any>();
      let pendingPresent = false;
      // The glue copies into `getCurrentTexture()` and never presents; the
      // copy is submitted before the current task ends, so a microtask is a
      // safe point to hand the frame to the compositor.
      this.ctx = {
        // The glue reaches back through the context to size the canvas.
        canvas: this,
        configure: (cfg) => {
          this.cfg = cfg;
          const d: any = cfg?.device;
          if (this.host) (this.host as any).gpuDevice = d;
          if (d && !watched.has(d)) {
            watched.add(d);
            d.lost?.then((info: any) => {
              log("GPU device lost", info?.reason ?? "", info?.message ?? "");
              this.host?.deviceLost?.(info);
            });
          }
          if (Deno.env.get("WC_SIZES") === "1") log("configure", "surface", raw.getConfiguration?.() ?? "", "canvas", this.width, this.height, "client", this.clientWidth, this.clientHeight, JSON.stringify(cfg));
          raw.configure(cfg);
        },
        unconfigure: () => raw.unconfigure?.(),
        getCurrentTexture: () => {
          let tex;
          try { tex = raw.getCurrentTexture(); }
          catch (e) {
            if (/device.{0,20}lost|lost.{0,20}device/i.test(String(e))) this.host?.deviceLost?.(e);
            throw e;
          }
          this.host?.frame?.();
          if (Deno.env.get("WC_SIZES") === "1") log("texture", tex.width, tex.height, "surface", surface.width, surface.height, "canvas", this.width, this.height, "client", this.clientWidth, this.clientHeight);
          const host: any = this.host;
          if (host?.capture) {
            const cap = host.capture, d = this.cfg?.device, cw = tex.width, ch = tex.height;
            host.capture = null;
            // Runs after the glue submits its copy, before present clears it.
            queueMicrotask(() => { try { cap(tex, d, cw, ch); } catch (e) { log("capture failed", String(e)); } });
          }
          if (!pendingPresent) {
            pendingPresent = true;
            queueMicrotask(() => {
              pendingPresent = false;
              try { surface.present(); } catch { /* already presented this frame */ }
            });
          }
          return tex;
        },
      };
      return this.ctx;
    }
    if (kind === "webgl2") return (this.gl ||= makeGL(this));
    if (kind === "webgl") return null;
    if (kind === "2d") return make2D(this);
    return null;
  }
  requestPointerLock() { (globalThis as any).document?.__lock?.(this); return Promise.resolve(); }
  toDataURL() { return "data:image/png;base64,"; }
  toBlob(cb: (b: Blob) => void) { cb(new Blob()); }
}

export interface Page {
  canvas: Canvas;
  document: any;
  cookie: { get(): string; set(v: string): void };
}

/**
 * Install the browser surface. `kind` is "main" or "worker"; `host` is set on
 * the main thread of the desktop app and absent when running headless.
 */
export function install(kind: "main" | "worker", host?: Host, opts: { place?: number; cookie?: string; renderer?: string } = {}): Page {
  const worker = kind === "worker";
  const stubImports = Deno.env.get("WC_STUB_IMPORTS");
  if (stubImports) {
    try { (globalThis as any).__STUB_IMPORTS = JSON.parse(stubImports); } catch {}
  }
  (globalThis as any).__wcRnaLog = Deno.env.get("WC_RNA_LOG") === "1";
  (globalThis as any).__wcRnaDump = Deno.env.get("WC_RNA_DUMP") === "1";
  (globalThis as any).__wcProgress = (m: string) => host?.progress?.(m);
  // A WebTransport handshake that never produces server data is a dead server
  // (old RCC build), not a transport bug. The engine lives in the worker, so
  // forward the signal to main through the patched Worker channel.
  (globalThis as any).__wcHandshakeFail = (m: string) => {
    if (worker) (globalThis as any).postMessage?.({ __handshakeFailed: m });
    else host?.handshakeFail?.(m);
  };

  // The global scope is already an EventTarget (and Deno dispatches worker
  // messages through it), so leave addEventListener alone and dispatch events.
  if (!worker) (globalThis as any).window = globalThis;
  (globalThis as any).self = globalThis;
  // Trace WebTransport so a stalled replication is diagnosable.
  const WT: any = (globalThis as any).WebTransport;
  if (WT && Deno.env.get("WC_WT_PASSTHROUGH") !== "1") {
    (globalThis as any).WebTransport = class extends WT {
      getStats() {
        if (Deno.env.get("WC_REAL_STATS") === "1") {
          return WT.prototype.getStats.call(this);
        }
        // Deno returns all-zero counters; the engine's replication scheduler
        // reads these every tick and treats zero as a stalled link. Report
        // plausible values that keep increasing, so progress checks see a live
        // link (static counters look stalled to a delta-based scheduler).
        const s: any = (this as any).__wcStats ??= { n: 0, sent: 0, recv: 0 };
        s.n++; s.sent += 137; s.recv += 149;
        return Promise.resolve({
          smoothedRtt: 0.02, minRtt: 0.015, rttVariation: 0.002,
          estimatedSendRate: 2_000_000, atSendCapacity: false,
          packetsSent: 100 + s.n, bytesSent: 20000 + s.sent, bytesSentOverhead: 500,
          packetsReceived: 100 + s.n, bytesReceived: 20000 + s.recv,
          packetsLost: 0, bytesLost: 0,
          datagrams: { droppedIncoming: 0, expiredIncoming: 0, expiredOutgoing: 0, lostOutgoing: 0 },
        });
      }
      constructor(url: any, opts: any) {
        // rwebtransport supports Origin/custom headers on the Extended CONNECT;
        // Deno's built-in ignores these extra dictionary members.
        const o: any = opts ? { ...opts } : {};
        const origin = Deno.env.get("WC_WT_ORIGIN_HDR");
        if (origin) o.origin = origin;
        const ua = Deno.env.get("WC_WT_UA_HDR");
        if (ua) o.headers = { ...(o.headers ?? {}), "user-agent": ua };
        if (Deno.env.get("WC_WT_CHROME_HDRS") === "1") {
          o.headers = {
            ...(o.headers ?? {}),
            "sec-webtransport-http3-draft02": "1",
            "user-agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36",
          };
        }
        super(url, o);
        console.error("[wt ] new", String(url), "hashes", opts?.serverCertificateHashes?.length ?? 0, "unreliable", opts?.requireUnreliable, "cc", opts?.congestionControl);
        // Roblox servers that accept the browser route open a unidirectional
        // stream and send real replication data within a few seconds. Broken
        // (older RCC build) servers only answer with 32-byte keepalives and
        // never produce a stream; Deno's WebTransport does not enforce Chrome's
        // 4s idle timeout, so ready() stays resolved and the UI hangs forever.
        // Detect that here and ask the watchdog to rejoin onto another server.
        const handshakeMs = Number(Deno.env.get("WC_WT_HANDSHAKE_MS") ?? 6000);
        const minData = Number(Deno.env.get("WC_WT_MIN_DATA") ?? 64);
        let streamAccepted = false;
        let failed = false;
        let failTimer: ReturnType<typeof setTimeout> | undefined;
        const markAlive = (why: string) => {
          if (failed || streamAccepted) return;
          streamAccepted = true;
          clearTimeout(failTimer);
          console.error("[wt ] handshake alive:", why);
        };
        const signalFail = (why: string) => {
          if (failed || streamAccepted) return;
          failed = true;
          clearTimeout(failTimer);
          console.error("[wt ] HANDSHAKE FAIL:", why, "- requesting rejoin");
          (globalThis as any).__wcHandshakeFail?.(why);
        };
        if (handshakeMs > 0) {
          failTimer = setTimeout(() => signalFail(`no server data after ${handshakeMs}ms`), handshakeMs);
        }
        const dg0: any = this.datagrams;
        if (dg0?.readable?.getReader) {
          const gr0 = dg0.readable.getReader.bind(dg0.readable);
          dg0.readable.getReader = () => {
            const r = gr0();
            const rd = r.read.bind(r);
            r.read = async () => {
              const v = await rd();
              if (!v.done) {
                const n = v.value?.byteLength ?? 0;
                console.error("[wt ] incoming datagram", n);
                if (n > minData) markAlive(`datagram ${n}B`);
              }
              return v;
            };
            return r;
          };
        }
        const iu0: any = this.incomingUnidirectionalStreams;
        if (iu0?.getReader) {
          const gr = iu0.getReader.bind(iu0);
          iu0.getReader = () => {
            const r = gr();
            const read = r.read.bind(r);
            r.read = async () => {
              const v = await read();
              if (!v.done) {
                console.error("[wt ] incoming stream accepted");
                markAlive("server stream");
              }
              return v;
            };
            return r;
          };
        }
        this.ready.then(
          () => {
            console.error("[wt ] ready", String(url), "maxDG", this.datagrams?.maxDatagramSize);
            let streams = 0, writes = 0;
            const mk = this.createUnidirectionalStream?.bind(this);
            if (mk) this.createUnidirectionalStream = () => {
              const p = mk();
              const sid = ++streams;
              if (sid <= 5) console.error("[wt ] stream create", sid);
              p.then((s: any) => {
                if (!s?.getWriter) return;
                const gw = s.getWriter.bind(s);
                s.getWriter = () => {
                  const w = gw();
                  const wr = w.write.bind(w);
                  w.write = (chunk: any) => {
                    ++writes;
                    if (Deno.env.get("WC_WT_DUMP") === "1") {
                      const b = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk?.buffer ?? chunk ?? []);
                      console.error("[wtdump]", sid, writes, b.byteLength,
                        Array.from(b).map((x) => x.toString(16).padStart(2, "0")).join(" "));
                    } else if (writes <= 8) console.error("[wt ] stream write", sid, writes, chunk?.byteLength);
                    return wr(chunk);
                  };
                  return w;
                };
              }).catch(() => {});
              return p;
            };
            const dg = this.datagrams;
            if (dg) {
              let n = 0;
              const orig = dg.writable?.getWriter?.bind(dg.writable);
              if (orig) dg.writable.getWriter = () => {
                const w = orig();
                const write = w.write.bind(w);
                w.write = (chunk: any) => {
                  ++n;
                  if (Deno.env.get("WC_WT_DUMP") === "1") {
                    const b = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk?.buffer ?? chunk ?? []);
                    console.error("[wtdg]", n, b.byteLength, Array.from(b.slice(0, 64)).map((x) => x.toString(16).padStart(2, "0")).join(" "));
                  } else if (n <= 5) console.error("[wt ] datagram write", n, chunk?.byteLength);
                  const p = write(chunk);
                  p.then(() => console.error("[wt ] datagram ACK", n),
                    (e: any) => console.error("[wt ] datagram WRITE ERR", n, e?.name, e?.message));
                  return p;
                };
                return w;
              };
            }
          },
          (e: any) => {
            console.error("[wt ] ready FAILED", e?.name, e?.message);
            signalFail(`ready rejected: ${e?.name ?? "error"}`);
          },
        );
        this.closed.then((e: any) => {
          console.error("[wt ] closed", e?.closeCode, e?.reason);
          clearTimeout(failTimer);
        }).catch(() => {});
      }
    };
  } else {
    console.error("[wt ] WebTransport unavailable");
  }
  // Log the engine -> page lifecycle callbacks. If any of these never fire,
  // the loading screen stays up even though the network join succeeded.
  if (!worker) {
    let hook: any, joined: any, status: any;
    const wrap = (name: string, store: (f: any) => void, get: () => any) =>
      Object.defineProperty(globalThis, name, {
        configurable: true,
        get: () => get(),
        set: (f: any) => {
          console.error("[env] hook installed:", name);
          store((...a: any[]) => {
            console.error("[env] hook CALLED:", name, JSON.stringify(a)?.slice(0, 300));
            if (name === "__onEngineLoadingStatus" && Deno.env.get("WC_FORCE_DISMISS") !== "0" &&
                /Loading experience/.test(JSON.stringify(a))) {
              setTimeout(() => (globalThis as any).__onDefaultLoadingScreenRemoved?.({}), 3000);
            }
            return f(...a);
          });
        },
      });
    wrap("__onDefaultLoadingScreenRemoved", (f) => (hook = f), () => hook);
    wrap("__onExperienceJoined", (f) => (joined = f), () => joined);
    wrap("__onEngineLoadingStatus", (f) => (status = f), () => status);
    // WasmPlayer.js does not install this one, so provide a listener.
    (globalThis as any).__onExperienceJoined = () => {};
  }
  // The glue gates its JSPI (fiber) machinery on this interface existing.
  if (!(globalThis as any).WorkerGlobalScope) (globalThis as any).WorkerGlobalScope = class WorkerGlobalScope {};

  const canvas = new Canvas(host);
  (globalThis as any).HTMLElement = El;
  (globalThis as any).HTMLCanvasElement = Canvas;
  (globalThis as any).HTMLVideoElement = class extends El {};
  (globalThis as any).HTMLAudioElement = class extends El {};
  (globalThis as any).HTMLScriptElement = class extends El { src = ""; onload: any = null; onerror: any = null; };
  (globalThis as any).WebGLRenderingContext = class {};
  (globalThis as any).Image = class extends El { set src(v: string) { (this as any)._src = v; } get src() { return (this as any)._src || ""; } };

  // Audio is a stub for now: a ScriptProcessor the engine can mix into, with
  // no device behind it. `audio.ts` replaces this with a real AudioContext.
  (globalThis as any).AudioContext = class {
    sampleRate = 48000;
    currentTime = 0;
    state = "running";
    destination = { channelCount: 2, maxChannelCount: 2, connect() {}, disconnect() {} };
    audioWorklet = { addModule: async () => {} };
    resume() { this.state = "running"; return Promise.resolve(); }
    suspend() { this.state = "suspended"; return Promise.resolve(); }
    close() { return Promise.resolve(); }
    createGain() { return { gain: { value: 1 }, connect() {}, disconnect() {} }; }
    createBufferSource() { return { buffer: null, connect() {}, disconnect() {}, start() {}, stop() {} }; }
    createScriptProcessor(_a: number, _b: number, c: number) {
      const size = [_a, _b, c].find((x) => x >= 256) || 4096;
      const n: any = { bufferSize: size, onaudioprocess: null, connect() {}, disconnect() {} };
      const buf = () => ({ numberOfChannels: 2, length: size, sampleRate: 48000, getChannelData: () => new Float32Array(size) });
      setInterval(() => n.onaudioprocess?.({ outputBuffer: buf(), inputBuffer: buf() }), Math.max(5, (size / 48) | 0));
      return n;
    }
  };
  (globalThis as any).webkitAudioContext = (globalThis as any).AudioContext;

  const screenSize = () => ({
    width: host ? host.width() : 1280,
    height: host ? host.height() : 720,
    dpr: host ? host.dpr() : 1,
  });
  Object.defineProperties(globalThis, {
    devicePixelRatio: { configurable: true, get: () => screenSize().dpr },
    innerWidth: { configurable: true, get: () => screenSize().width },
    innerHeight: { configurable: true, get: () => screenSize().height },
    outerWidth: { configurable: true, get: () => screenSize().width },
    outerHeight: { configurable: true, get: () => screenSize().height },
    screen: { configurable: true, get: () => ({ ...screenSize(), availWidth: screenSize().width, availHeight: screenSize().height, colorDepth: 24, pixelDepth: 24 }) },
    scrollX: { configurable: true, value: 0 },
    scrollY: { configurable: true, value: 0 },
    pageXOffset: { configurable: true, value: 0 },
    pageYOffset: { configurable: true, value: 0 },
    crossOriginIsolated: { configurable: true, value: true },
  });

  const place = opts.place ?? 189707;
  const search = worker ? "" : `?placeId=${place}` + (opts.renderer ? `&renderer=${opts.renderer}` : "");
  Object.defineProperty(globalThis, "location", {
    configurable: true,
    value: {
      href: PAGE, origin: ORIGIN, protocol: "https:", host: "play.roblox.com", hostname: "play.roblox.com",
      port: "", pathname: "/games/start", search, hash: "", ancestorOrigins: [],
      reload() {}, assign() {}, replace() {}, toString() { return this.href; },
    },
  });

  (globalThis as any).onerror = null;
  (globalThis as any).addEventListener("error", (e: any) => log("window error", e.message || e));
  (globalThis as any).open = () => null;
  (globalThis as any).close = () => {};
  (globalThis as any).focus = () => {};
  (globalThis as any).alert = () => {};
  (globalThis as any).getComputedStyle = () => ({ getPropertyValue: () => "" });
  (globalThis as any).matchMedia = (q: string) => ({
    media: q,
    matches: q.includes("pointer: fine") || q.includes("prefers-color-scheme: light"),
    addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {},
  });
  (globalThis as any).requestAnimationFrame = (f: any) => setTimeout(() => f(performance.now()), 16);
  (globalThis as any).cancelAnimationFrame = (i: any) => clearTimeout(i);
  (globalThis as any).ResizeObserver = class {
    fn: any;
    els: any[] = [];
    constructor(fn: any) { this.fn = fn; }
    observe(el: any) { this.els.push(el); observers.push(this); }
    unobserve() {}
    disconnect() { observers = observers.filter((o) => o !== this); }
  };

  const store = new Map<string, string>();
  (globalThis as any).localStorage = {
    getItem: (k: string) => store.get(k) ?? null,
    setItem: (k: string, v: any) => store.set(k, String(v)),
    removeItem: (k: string) => store.delete(k),
    clear: () => store.clear(),
    key: (i: number) => [...store.keys()][i] ?? null,
    get length() { return store.size; },
  };

  const opfs = makeOPFS();
  let observers: any[] = [];

  const gpu = (globalThis as any).navigator?.gpu;

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
    gpu,
    doNotTrack: null,
    userAgentData: { mobile: false, platform: "Linux x86_64", brands: [{ brand: "Chromium", version: "157" }] },
    storage: {
      getDirectory: async () => opfs,
      estimate: async () => ({ usage: 0, quota: 1 << 30 }),
      persist: async () => true,
      persisted: async () => true,
    },
    locks: { request: async (_n: string, o: any, cb?: any) => (typeof o === "function" ? o({ mode: "exclusive" }) : cb({ mode: "exclusive" })) },
    keyboard: { getLayoutMap: async () => new Map() },
    clipboard: { writeText: async () => {}, readText: async () => "" },
    share: async () => {},
    canShare: () => false,
    getGamepads: () => [],
    userActivation: { isActive: true, hasBeenActive: true },
    permissions: { query: async () => ({ state: "granted" }) },
  };
  Object.defineProperty(globalThis, "navigator", { value: nav, configurable: true });

  // A document.cookie jar. Assignments are `k=v` (possibly one pair); the
  // engine sets tracker cookies and must not clobber the session cookie.
  const jar = new Map<string, string>();
  if (opts.cookie) jar.set(".ROBLOSECURITY", opts.cookie);
  jar.set("RBXEventTrackerV2", "browserid=wc");
  const cookie = {
    get: () => [...jar].map(([k, v]) => `${k}=${v}`).join("; "),
    set: (v: string) => {
      const pair = String(v).split(";")[0];
      const at = pair.indexOf("=");
      if (at < 0) return;
      if (pair.slice(at + 1) === "") jar.delete(pair.slice(0, at).trim());
      else jar.set(pair.slice(0, at).trim(), pair.slice(at + 1));
    },
  };

  class Doc extends EventTarget {}
  const doc: any = new Doc();
  Object.assign(doc, {
    documentElement: new El("html"),
    head: new El("head"),
    body: new El("body"),
    title: "Roblox",
    readyState: "complete",
    visibilityState: "visible",
    hidden: false,
    fullscreenElement: null,
    currentScript: { src: "" },
    _ids: new Map<string, any>([["canvas", canvas]]),
    createElement: (tag: string) => (tag === "canvas" ? new Canvas(host) : new El(tag)),
    createElementNS: (_ns: string, tag: string) => (tag === "canvas" ? new Canvas(host) : new El(tag)),
    getElementById: (id: string) => doc._ids.get(id) ?? null,
    querySelector: (s: string) => doc._ids.get(s.replace(/^#/, "")) ?? null,
    querySelectorAll: () => [],
    getElementsByTagName: () => [],
    hasFocus: () => true,
    pointerLockElement: null,
    fullscreenEnabled: true,
    fonts: { ready: Promise.resolve(), add() {}, check: () => true, load: async () => [] },
    exitFullscreen: async () => {},
    exitPointerLock() {
      if (!doc.pointerLockElement) return;
      doc.pointerLockElement = null;
      doc.dispatchEvent(ev("pointerlockchange"));
    },
    __lock(target: any) {
      doc.pointerLockElement = target;
      doc.dispatchEvent(ev("pointerlockchange"));
    },
  });
  doc.body.appendChild(canvas);
  for (const id of ["status", "server", "proxyenabled", "loading-overlay", "input-container", "input-textarea", "input-input"]) doc._ids.set(id, new El("div"));
  Object.defineProperty(doc, "cookie", { get: cookie.get, set: cookie.set });
  (globalThis as any).document = doc;

  // Real window input is the page's input. Pointer events go to the canvas,
  // as they do in a browser; keys/focus go to the window and document.
  if (host) {
    const canvasTypes = ["pointerdown", "pointerup", "pointermove", "mousedown", "mouseup", "mousemove", "click", "dblclick", "wheel", "contextmenu", "touchstart", "touchend", "touchmove", "touchcancel"];
    const trace = (t: string, e: any) => {
      if (traceInput()) log("input", t, "key=", e.key, "code=", e.code, "kc=", keyCodeOf(e), "xy=", e.clientX, e.clientY, "btn=", e.button, "mov=", e.movementX, e.movementY);
    };
    let lastX = 0, lastY = 0, havePos = false, buttons = 0;
    // A native context menu grabs the pointer on some window backends, after
    // which mousemove stops being delivered at all. Suppress it.
    host.on("contextmenu", (e: any) => { try { e.preventDefault(); } catch { /* not cancelable */ } });

    for (const t of ["keydown", "keyup", "focus", "blur", "mouseleave", "mouseenter"]) {
      host.on(t, (e) => {
        if (t === "blur" || t === "mouseleave") buttons = 0;
        const c = cloneEvent(e);
        trace(t, c);
        globalThis.dispatchEvent(c);
        doc.dispatchEvent(cloneEvent(e));
      });
    }
    // DOM `button` indices 0,1,2 are left,middle,right but `buttons` bits
    // are 1,4,2 respectively. Getting this wrong makes the engine think the
    // middle button is held after a right click.
    const BUTTON_BITS = [1, 4, 2, 8, 16];
    for (const t of canvasTypes) {
      host.on(t, (e: any) => {
        const x = e.clientX, y = e.clientY;
        const finite = Number.isFinite(x) && Number.isFinite(y);
        const dx = finite && havePos ? x - lastX : 0;
        const dy = finite && havePos ? y - lastY : 0;
        lastX = x; lastY = y; havePos = finite;
        const bit = BUTTON_BITS[e.button ?? 0] ?? 1;
        if (t === "mousedown" || t === "pointerdown") buttons |= bit;
        if (t === "mouseup" || t === "pointerup") buttons &= ~bit;
        if (t === "mouseleave") buttons = 0;
        const held = Number.isInteger(e.buttons) ? e.buttons : buttons;
        const c = cloneEvent(e, { movementX: dx, movementY: dy, buttons: held, target: canvas });
        trace(t, c);
        canvas.dispatchEvent(c);
        // In a browser a click on the canvas bubbles through body -> document
        // -> window. Roblox's UI (including the post-join retry/leave buttons)
        // listens on document/window in places, so deliver the same event up
        // that chain. Only the canvas originally received it, which made those
        // buttons look dead.
        if (t !== "mousemove" && t !== "pointermove" && t !== "mouseleave") {
          for (const [node, label] of [[doc.body, "body"], [doc, "document"], [globalThis, "window"]] as [EventTarget, string][]) {
            const b = cloneEvent(e, { movementX: dx, movementY: dy, buttons: held, target: canvas, currentTarget: label });
            node.dispatchEvent(b);
          }
        }
      });
    }
    host.on("resize", (e) => {
      trace("resize", e);
      const [w, h] = [host.width(), host.height()];
      for (const o of observers) {
        try { o.fn([{ target: o.els[0], contentRect: { width: w, height: h } }]); } catch { /* ignore */ }
      }
    });
  }

  const realFetch = globalThis.fetch;
  (globalThis as any).fetch = async (input: any, init: any = {}) => {
    const url = typeof input === "string" ? input : input.url;
    const h = new Headers(init.headers ?? (typeof input === "object" ? input.headers : undefined));
    patchHeaders(url, h);
    return realFetch(input, { ...init, headers: h });
  };
  (globalThis as any).XMLHttpRequest = XHR;

  // Page scripts load the way a browser loads them: append a <script>, then
  // run it with `document.currentScript` pointing at the tag.
  const runScript = async (tag: any) => {
    try {
      const r = await fetch(tag.src);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      doc.currentScript = tag;
      (0, eval)((await r.text()) + `\n//# sourceURL=${tag.src}`);
      tag.onload?.();
    } catch (e) {
      log("script failed", tag.src, e);
      tag.onerror?.(e);
    } finally {
      doc.currentScript = null;
    }
  };
  const realCreate = doc.createElement;
  doc.createElement = (t: string) => (t === "script" ? new (globalThis as any).HTMLScriptElement() : realCreate(t));
  for (const host of [doc.head, doc.body]) {
    const append = host.appendChild.bind(host);
    host.appendChild = (c: any) => { append(c); if (c instanceof (globalThis as any).HTMLScriptElement && c.src) void runScript(c); return c; };
  }
  (globalThis as any).__runScript = runScript;

  patchEngineWorkers(worker ? "worker" : "main");

  if (typeof (WebAssembly as any).Suspending === "undefined") throw new Error("no JSPI");
  wrapJspi();
  return { canvas, document: doc, cookie };
}

/** Set the headers Roblox requires, on Roblox requests only. */
export function patchHeaders(url: string, h: Headers) {
  let host = "";
  try { host = new URL(url).hostname; } catch { return; }
  if (!/(^|\.)(roblox\.com|rbxcdn\.com|robloxlabs\.com)$/.test(host)) return;
  if (!h.has("cookie")) h.set("cookie", (globalThis as any).document.cookie);
  if (!h.has("user-agent")) h.set("user-agent", UA);
  h.set("origin", ORIGIN);
  h.set("referer", PAGE + (globalThis as any).location.search);
  h.set("rbx-user-agent", Deno.env.get("WC_RBX_UA") ?? "Roblox/Darwin");
}

/**
 * Engine workers are created with the engine's own script URL. Point them at
 * our worker module, remembering the URL in the fragment for `importScripts`.
 */
let workerUrl = "";
function patchEngineWorkers(kind: "main" | "worker") {
  const DenoWorker = (globalThis as any).Worker;
  (globalThis as any).Worker = class {
    _w: any;
    _ready = false;
    _q: [any, any][] = [];
    onmessage: ((e: any) => void) | null = null;
    onerror: ((e: any) => void) | null = null;
    constructor(url: string, opts: any = {}) {
      workerUrl ||= String(url);
      const u = new URL("./worker.ts", import.meta.url);
      u.hash = encodeURIComponent(String(url));
      this._w = new DenoWorker(u, { type: "module", name: opts.name, deno: { permissions: "inherit" } });
      this._w.onerror = (e: any) => {
        console.error("[worker] error", e?.message ?? String(e));
        try { e?.preventDefault?.(); } catch {}
        this.onerror?.(e);
        return true;
      };
      this._w.onmessage = (e: any) => {
        if (e.data?.__ready) {
          if (Deno.env.get("WC_NET_DEBUG") === "1") console.error("[worker] __ready received");
          this._ready = true;
          for (const [m, t] of this._q) this._w.postMessage(m, t);
          this._q.length = 0;
          return;
        }
        if (e.data?.__log) { log("worker:", e.data.__log); return; }
        if (e.data?.__error) { console.error("[worker] failed:", e.data.__error); return; }
        if (e.data?.__handshakeFailed) { (globalThis as any).__wcHandshakeFail?.(e.data.__handshakeFailed); return; }
        this.onmessage?.(e);
      };
    }
    postMessage(m: any, t: any) {
      if (!this._ready) { this._q.push([m, t]); return; }
      try { this._w.postMessage(m, t); } catch (e) { log("post failed", e); }
    }
    addEventListener(t: string, f: any) { if (t === "message") this.onmessage = f; }
    removeEventListener() {}
    terminate() { this._w.terminate(); }
  };
  if (kind === "worker") (globalThis as any).postMessage ??= (m: any) => (globalThis as any).__post?.(m);
}

/** Report a throw inside a suspending import through the engine's stack. */
function wrapJspi() {
  const S = (WebAssembly as any).Suspending;
  const P = (WebAssembly as any).promising;
  Object.defineProperty(WebAssembly, "Suspending", {
    configurable: true,
    value: function (fn: any) {
      return new S(function (...a: any[]) {
        try { return fn(...a); } catch (e) {
          if (e instanceof (WebAssembly as any).Exception) log("threw in import", fn.name || "?");
          throw e;
        }
      });
    },
  });
  Object.defineProperty(WebAssembly, "promising", {
    configurable: true,
    value: function (fn: any) {
      const g = P(fn);
      return function (...a: any[]) {
        try { return g(...a); } catch (e) {
          if (e instanceof (WebAssembly as any).Exception) log("threw in export", fn.name || "?");
          throw e;
        }
      };
    },
  });
}

export { workerUrl };
