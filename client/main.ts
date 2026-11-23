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

const base = await resolveEngine();
Deno.env.set("WC_ENGINE", base);
Deno.env.set("WC_PAGE_ASSETS", await resolvePage());
installFetch();

const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
if (!adapter) throw new Error("no WebGPU adapter");
const device = await adapter.requestDevice({ requiredFeatures: [...adapter.features], requiredLimits: adapter.limits as any });

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

// The watchdog lives on its own thread: the engine can block the main one.
const watchdog = new Worker(new URL("./page/watchdog.ts", import.meta.url), {
  type: "module",
  deno: { permissions: "inherit" },
});
watchdog.postMessage({
  exe: Deno.execPath(),
  args: Deno.args,
  timeout: Number(Deno.env.get("WC_WATCHDOG_MS") ?? 30000),
});

const host = {
  surface,
  frame: () => watchdog.postMessage({ frame: 1 }),
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
const ctx = page.canvas.getContext("webgpu");
ctx.configure({ device, format: navigator.gpu.getPreferredCanvasFormat(), alphaMode: "opaque" });

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
