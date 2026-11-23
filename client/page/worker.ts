// Engine worker: a real DedicatedWorkerGlobalScope running the stock glue.
//
// `stub.js` only does `importScripts(engine)`, so the worker boots by eval'ing
// that stub with `importScripts` wired to our cache.
import { install } from "./env.ts";
import { installFetch, canonical, mirrorFile } from "./net.ts";

installFetch();
const stubUrl = decodeURIComponent(new URL(import.meta.url).hash.slice(1)) || "https://play.roblox.com/stub.js";
const cookieFile = Deno.env.get("WC_COOKIE_FILE");
let cookie = "";
if (cookieFile) {
  try { cookie = (await Deno.readTextFile(cookieFile)).trim().replace(/^\.ROBLOSECURITY=/, ""); } catch { /* none */ }
}

install("worker", undefined, { cookie });
globalThis.name = "em-pthread";
Object.defineProperty(globalThis, "location", { configurable: true, value: new URL(stubUrl) });

(globalThis as any).postMessage ??= (m: any) => (globalThis as any).__post?.(m);

async function source(url: string): Promise<string> {
  const r = await fetch(canonical(new URL(url, stubUrl).href));
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.text();
}

(globalThis as any).importScripts = (...urls: string[]) => {
  // importScripts is synchronous in a browser; the engine only calls it with
  // local resources, which are already in the mirror by the time a worker runs.
  for (const u of urls) {
    const path = canonical(new URL(u, stubUrl).href);
    const hit = syncSource(path);
    if (hit === null) throw new Error(`importScripts: ${u} is not mirrored`);
    (0, eval)(`${hit}\n//# sourceURL=${path}`);
  }
};

/** Synchronous mirror read: workers only import what the main thread fetched. */
function syncSource(url: string): string | null {
  try { return Deno.readTextFileSync(mirrorFile(url)); } catch { return null; }
}

const stub = await source(stubUrl);
(0, eval)(`${stub}\n//# sourceURL=${stubUrl}`);
(globalThis as any).postMessage({ __ready: 1 });
