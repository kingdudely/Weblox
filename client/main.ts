// Alcohol: the Roblox web client in a real window.
//
// The engine is Roblox's own web build (the same files play.roblox.com ships)
// running on Deno. This file only supplies what a window supplies: a surface,
// input, a title, and a place to put the page.
import { install } from "./page/env.ts";
import { resolveEngine, resolvePage, installFetch, canonical } from "./page/net.ts";

const place = Number(Deno.args[0] ?? Deno.env.get("WC_PLACE") ?? 1818);
const cookieFile = Deno.env.get("WC_COOKIE_FILE") ?? "/home/john/king.txt";
let cookie = "";
try { cookie = (await Deno.readTextFile(cookieFile)).trim().replace(/^\.ROBLOSECURITY=/, ""); } catch { console.error("[main] no cookie at", cookieFile); }

// Optionally use a Rust/wtransport-backed WebTransport implementation instead
// of Deno's built-in one (which omits Origin/User-Agent on CONNECT).
if (Deno.env.get("WC_WT_LIB") === "1") {
  const m: any = await import("npm:@webtransport-bun/webtransport@0.3.0");
  (globalThis as any).WebTransport = m.WebTransport;
  console.error("[wt ] using npm WebTransport library");
} else if (Deno.env.get("WC_WT_FC") === "1") {
  await import("npm:@fails-components/webtransport-transport-http3-quiche@1.6.12");
  const m: any = await import("npm:@fails-components/webtransport@1.6.12");
  (globalThis as any).WebTransport = m.WebTransport;
  console.error("[wt ] using libquiche WebTransport");
} else if (Deno.env.get("WC_WT_RW") === "1") {
  // quiche + BoringSSL (Chromium's TLS stack) with Origin/custom headers on
  // the Extended CONNECT -- the one difference from Chrome we can still test.
  const m: any = await import("npm:rwebtransport@0.2.5");
  (globalThis as any).WebTransport = m.WebTransport;
  console.error("[wt ] using rwebtransport (Origin-capable)");
}

const base = await resolveEngine();
Deno.env.set("WC_ENGINE", base);
Deno.env.set("WC_PAGE_ASSETS", await resolvePage());
installFetch();

// Deno requires requestAdapter() before a window surface can be created. The
// engine creates its own device from `navigator.gpu` (WasmPlayer.js) and the
// canvas is configured with that device, so don't keep a second device here.
const adapter = await (globalThis as any).navigator?.gpu?.requestAdapter({ powerPreference: "high-performance" });
if (!adapter) throw new Error("no WebGPU adapter");

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
  if (Deno.env.get("WC_SIZES") === "1") console.error("[main] window", w, h, "surface", surface.width, surface.height);
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

// The watchdog lives on its own thread: the engine can block the main one.
const watchdog = new Worker(new URL("./page/watchdog.ts", import.meta.url), {
  type: "module",
  deno: { permissions: "inherit" },
});
watchdog.postMessage({
  exe: Deno.execPath(),
  args: Deno.args,
  timeout: Number(Deno.env.get("WC_WATCHDOG_MS") ?? 18000),
});

const host = {
  surface,
  frame: () => watchdog.postMessage({ frame: 1 }),
  deviceLost: () => watchdog.postMessage({ lost: 1 }),
  progress: (m: string) => watchdog.postMessage({ progress: m }),
  handshakeFail: (m: string) => {
    console.error("[main] handshake fail -> rejoin:", m);
    watchdog.postMessage({ handshakeFail: m });
  },
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

if (Deno.env.get("WC_SELFTEST") === "1") {
  setTimeout(() => {
    for (const [type, init] of [
      ["keydown", { key: "w", code: "KeyW" }],
      ["keyup", { key: "w", code: "KeyW" }],
      ["mousemove", { clientX: 100, clientY: 100 }],
      ["mousemove", { clientX: 140, clientY: 120 }],
      ["mousedown", { clientX: 140, clientY: 120, button: 0 }],
      ["mouseup", { clientX: 140, clientY: 120, button: 0 }],
      ["wheel", { clientX: 140, clientY: 120, deltaX: 0, deltaY: -3 }],
    ] as [string, any][]) {
      const G = globalThis as any;
      const C = type.startsWith("key") ? G.KeyboardEvent : type === "wheel" ? G.WheelEvent : G.MouseEvent;
      win.dispatchEvent(new (C as any)(type, init));
    }
  }, 4000);
}
win.addEventListener("close", () => Deno.exit(0));
win.setTitle(`${place} - Alcohol`);
setTimeout(() => {
  const m: any = (globalThis as any).Module ?? {};
  const keys = Object.keys(m).filter((k) => /load|gui|game|ready|default|screen/i.test(k));
  console.error("[module] lifecycle keys:", keys.join(","));
}, 14000);

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
