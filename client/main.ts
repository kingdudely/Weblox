// Alcohol: the Roblox web client in a real window.
//
// The engine is Roblox's own web build (the same files play.roblox.com ships)
// running on Deno. This file only supplies what a window supplies: a surface,
// input, a title, and a place to put the page.
import { install } from "./page/env.ts";
import { resolveEngine, resolvePage, installFetch, canonical } from "./page/net.ts";

const place = Number(Deno.args[0] ?? Deno.env.get("WC_PLACE") ?? 189707);
const cookieFile = Deno.env.get("WC_COOKIE_FILE") ?? "/home/john/mizg.txt";
let cookie = "";
try {
  cookie = (await Deno.readTextFile(cookieFile)).trim().replace(/^\.ROBLOSECURITY=/, "");
} catch {
  throw new Error(`no .ROBLOSECURITY cookie at ${cookieFile} — set WC_COOKIE_FILE`);
}
if (!cookie) throw new Error(`empty .ROBLOSECURITY cookie at ${cookieFile}`);

// Roblox/qdmux negotiates WebTransport draft-02, which Deno's built-in
// implementation cannot send (no CONNECT-header hook). The pure-TS module
// supplies that header over Deno's raw QUIC API. This is the only path.
const { DraftWebTransport } = await import("./page/draftwt.ts");
(globalThis as any).WebTransport = DraftWebTransport;

const base = await resolveEngine();
Deno.env.set("WC_ENGINE", base);
Deno.env.set("WC_PAGE_ASSETS", await resolvePage());
installFetch();

// Deno requires requestAdapter() before a window surface can be created. The
// engine creates its own device from `navigator.gpu` (WasmPlayer.js) and the
// canvas is configured with that device, so don't keep a second device here.
const adapter = await (globalThis as any).navigator?.gpu?.requestAdapter({ powerPreference: "high-performance" });
if (!adapter) throw new Error("no WebGPU adapter — this host requires a WebGPU-capable GPU/driver");

const win = new (Deno as any).BrowserWindow({ title: "Alcohol", width: 1280, height: 720 });
// The first BrowserWindow adopts the startup window, whose size the
// constructor cannot change; set it explicitly so client and surface agree.
win.setSize(1280, 720);
const surface = win.getNativeWindow();
// The surface is the window's content area. Keep it exactly the size of the
// client area; the compositor scales any mismatch, which looks like stretch.
const syncSurface = () => {
  const [w, h] = win.getSize();
  if (surface.width !== w || surface.height !== h) {
    surface.width = w;
    surface.height = h;
  }
};
syncSurface();
win.addEventListener("resize", syncSurface);

// Optional self-capture of the app's own rendered canvas, written to a local
// file. System/Wayland screenshot APIs are unavailable in this environment.
const dumpDest = Deno.env.get("WC_DUMP_FILE") ?? "/tmp/opencode/appdump.png";
const dumpFrame = Deno.env.get("WC_DUMP") === "1"
  ? async (tex: any, device: any, w: number, h: number) => {
    try {
      const G = globalThis as any;
      const bpr = Math.ceil((w * 4) / 256) * 256;
      const buf = device.createBuffer({ size: bpr * h, usage: G.GPUBufferUsage.COPY_DST | G.GPUBufferUsage.MAP_READ });
      const enc = device.createCommandEncoder();
      enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: bpr, rowsPerImage: h }, { width: w, height: h });
      device.queue.submit([enc.finish()]);
      await buf.mapAsync(G.GPUMapMode.READ);
      const raw = new Uint8Array(buf.getMappedRange()).slice();
      buf.unmap();
      const header = new TextEncoder().encode(`${w}x${h}\n`);
      const out = new Uint8Array(header.byteLength + raw.byteLength);
      out.set(header); out.set(raw, header.byteLength);
      await Deno.writeFile(dumpDest, out);
      console.error("[dump] wrote", dumpDest, w, h, bpr, raw.byteLength);
    } catch (e) {
      console.error("[dump] failed", String(e));
    }
  }
  : null;
if (dumpFrame) setTimeout(() => { (host as any).capture = dumpFrame; }, Number(Deno.env.get("WC_DUMP_MS") ?? 20000));

const host = {
  surface,
  width: () => surface.width,
  height: () => surface.height,
  dpr: () => 1,
  on: (type: string, fn: (e: any) => void) => {
    win.addEventListener(type, fn);
    return () => win.removeEventListener(type, fn);
  },
  setTitle: (t: string) => win.setTitle(t),
};

const page = install("main", host, { place, cookie });
// The engine configures this context with its own device during bootstrap.
page.canvas.getContext("webgpu");

win.addEventListener("close", () => Deno.exit(0));
win.setTitle(`${place} - Alcohol`);

// The start page's own globals.
(globalThis as any).robloxWasmHostConfig = {
  server: "prod",
  useCdnBinary: true,
  workerStubUrl: "https://play.roblox.com/stub.js",
};

const loader = canonical("WasmLoader.js");
console.error("[main] place", place, "engine", base);
const tag: any = new (globalThis as any).HTMLScriptElement();
tag.src = loader;
await (globalThis as any).__runScript(tag);
