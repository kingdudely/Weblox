import { parentPort, workerData } from "node:worker_threads";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { install, log } from "./env_deno.mjs";

globalThis.name = workerData.name;
install("worker");
globalThis.postMessage = (m, t) => { try { parentPort.postMessage(m, t); } catch (e) { log("post", e.message); } };

// A browser worker resolves `locateFile` output against its own script URL;
// here the module lives next to the host's copy.
const plainFetch = globalThis.fetch;
globalThis.fetch = async (input, init) => {
  const u = typeof input === "string" ? input : input.url;
  if (!/^https?:/.test(u)) {
    const p = new URL(u, "file:///home/john/rr/").pathname;
    try { return new Response(await Deno.readFile(p)); } catch (e) { log("fetch", u, e.message); }
  }
  return plainFetch(input, init);
};

const src = readFileSync(workerData.js, "utf8");
try {
  vm.runInThisContext(src, { filename: workerData.js });
} catch (e) {
  log("worker script threw", e.stack);
}

parentPort.on("message", (data) => {
  try { globalThis.onmessage?.({ data }); } catch (e) { log("worker onmessage threw", e.stack); }
});
