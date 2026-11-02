// ==UserScript==
// @name         Roblox Web Client -> RakNet (patched engine)
// @namespace    raknet-wasm-client
// @version      0.1
// @description  Loads a locally patched WebClient.wasm into play.roblox.com and forces the RakNet transport.
// @match        https://play.roblox.com/*
// @run-at       document-start
// @grant        GM_xmlhttpRequest
// @connect      127.0.0.1
// ==/UserScript==

// Why this exists:
//   The engine ships with WebTransport (RbxTransport) selected. RakNet is
//   compiled in but its libc socket layer is stubbed out, so it dies at
//   getaddrinfo. We serve a patched module from a local server and hand it to
//   the engine in place of the CDN copy. Everything else - join, assets,
//   challenge, rendering - stays the real client.
//
// Setup: install Tampermonkey, then run the local module server:
//     python3 tools/serve_wasm.py --file WebClient.patched.wasm --port 8765
// GM_xmlhttpRequest is what makes this work: page JS cannot fetch loopback
// from an https origin (mixed content) and the CDN copy is CORS-locked, but a
// userscript can. Tampermonkey is doing real work here, not just packaging.
(() => {
  if (globalThis.__rbxRaknetHook) return;
  globalThis.__rbxRaknetHook = 1;

  const CANDIDATES = ["https://127.0.0.1:8765/WebClient.wasm", "https://127.0.0.1:8765/engine/WebClient.wasm", "http://127.0.0.1:8765/WebClient.wasm"];

  // ---------------------------------------------------------------- flags --
  // Force the RakNet branch: the web client defaults to RbxTransport.
  const origFetch = self.fetch;
  self.fetch = function (input, init) {
    const url = typeof input === "string" ? input : (input && input.url) || "";
    const p = origFetch.call(self, input, init);
    if (/\/v2\/settings\/application\/WebClientApp/.test(url)) {
      return p.then(async (r) => {
        try {
          const j = await r.clone().json();
          j.applicationSettings = j.applicationSettings || {};
          j.applicationSettings.FFlagUseRbxTransportClient3 = "False";
          j.applicationSettings.FFlagUseRbxTransportServer = "False";
          console.log("[raknet] transport flag forced");
          return new Response(JSON.stringify(j), { status: 200, headers: { "content-type": "application/json" } });
        } catch (e) { return r; }
      });
    }
    return p;
  };

  // ----------------------------------------------------------- wasm bytes --
  let patched = null;              // Promise<WebAssembly.Module>
  const load = () => {
    if (patched) return patched;
    patched = (async () => {
      let buf = null, last = null;
      for (const url of CANDIDATES) {
        try {
          buf = await new Promise((res, rej) => {
            if (typeof GM_xmlhttpRequest === "function") {
              GM_xmlhttpRequest({
                method: "GET", url, responseType: "arraybuffer",
                onload: (r) => (r.status === 200 ? res(r.response) : rej(new Error("HTTP " + r.status))),
                onerror: rej,
              });
            } else {
              fetch(url).then((r) => (r.ok ? r.arrayBuffer() : Promise.reject(new Error("HTTP " + r.status)))).then(res, rej);
            }
          });
          break;
        } catch (e) { last = e; }
      }
      if (!buf) throw last || new Error("no local module");
      const mod = await WebAssembly.compile(buf);
      console.log("[raknet] patched engine compiled (" + buf.byteLength + " B)");
      return mod;
    })();
    patched.catch((e) => console.warn("[raknet] patched engine unavailable:", e && e.message));
    return patched;
  };
  load();

  // Swap the module the engine compiles/instantiates.
  const oInstantiate = WebAssembly.instantiate;
  const oStreaming = WebAssembly.instantiateStreaming;

  // Always instantiate OUR module, whatever the engine asked for: the CDN copy
  // and ours have identical structure, ours is the patched one.
  WebAssembly.instantiate = function (mod, imports) {
    return load().then((m) => oInstantiate.call(WebAssembly, m, imports));
  };

  WebAssembly.instantiateStreaming = function (source, imports) {
    return load().then((m) => {
      source && source.catch && source.catch(() => {});
      return oInstantiate.call(WebAssembly, m, imports);
    });
  };

  // Emscripten prefers instantiateStreaming when it can, but if the module is
  // precompiled by the page it calls instantiate(module, imports) - handled above.
  console.log("[raknet] hook installed");
})();
