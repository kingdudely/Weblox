// Worker side of the local page host: a real DedicatedWorkerGlobalScope.
// `stub.js` only does importScripts(engine), so eval the local copies.
import { install } from "../oracle/env_deno.mjs";

const LOCAL = "/tmp/opencode/page/";
const stubUrl = decodeURIComponent(new URL(import.meta.url).hash.slice(1)) || "https://play.roblox.com/x/stub.js";
const L = (...a: any[]) => { try { postMessage({ __log: a.join(" ") }); } catch {} };

const map = (u: string) => {
  let p = u;
  try { p = new URL(u, stubUrl).pathname; } catch { /* keep */ }
  return LOCAL + p.slice(p.lastIndexOf("/") + 1);
};

L("start", stubUrl, import.meta.url);
install("worker");
L("installed");
// a browser worker lives until terminated; Deno exits an empty loop
setInterval(() => {}, 1 << 30);
for (const k of ["error", "warn", "log"] as const) {
  const o = (console as any)[k].bind(console);
  (console as any)[k] = (...a: any[]) => { o(...a); L(k, a.map((x) => String(x)).join(" ").slice(0, 300)); };
}
globalThis.name = "em-pthread";
Object.defineProperty(globalThis, "location", { configurable: true, value: new URL(stubUrl) });

const realFetch = globalThis.fetch;
globalThis.fetch = async (input: any, init: any) => {
  const u = typeof input === "string" ? input : input.url;
  if (/setup\.rbxcdn\.com|play\.roblox\.com/.test(u)) {
    try { return new Response(await Deno.readFile(map(u))) } catch {}
  }
  return realFetch(input, init);
};

addEventListener("message", (e: any) => { if (e.data && e.data.cmd) L("cmd", e.data.cmd) });
(globalThis as any).importScripts = (...urls: string[]) => {
  for (const u of urls) (0, eval)(Deno.readTextFileSync(map(u)));
};

L("eval stub");
try { (0, eval)(Deno.readTextFileSync(map(stubUrl))); } catch (e) { L("eval threw", String((e as any)?.stack || e)); throw e }
L("stub done");
postMessage({ __ready: 1 });
