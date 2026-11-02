// ==UserScript==
// @name         Roblox Web Client -> RakNet (patched engine)
// @namespace    raknet-wasm-client
// @version      0.2
// @description  Feeds play.roblox.com a locally patched WebClient.wasm and forces the RakNet transport.
// @match        https://play.roblox.com/*
// @run-at       document-start
// @grant        GM_xmlhttpRequest
// @connect      127.0.0.1
// ==/UserScript==

// WHAT THIS DOES
//   The web client ships with WebTransport (RbxTransport) selected. RakNet is
//   compiled in but its libc socket layer is stubbed, so it dies at getaddrinfo.
//   This script swaps in a patched module (built by tools/patch.py) and flips
//   the transport flag. Join, assets, challenge and rendering stay the real
//   client - nothing is reimplemented.
//
// EXPECTED RESULT (current patch set: getaddrinfo + 2 errno stubs)
//   Before: getaddrinfo(...) failed:Error code - -4
//   After : RakNet Socket bound to invalid (privileged) port: 0
//   That difference proves the patched module is running. It will not join yet.
(() => {
  if (globalThis.__rbxRaknetHook) return;
  globalThis.__rbxRaknetHook = 1;

  const CANDIDATES = [
    "http://127.0.0.1:8765/WebClient.wasm",
    "http://127.0.0.1:8765/engine/WebClient.wasm",
  ];
  const hasGM = typeof GM_xmlhttpRequest === "function";

  // -------------------------------------------------------------- 1. flags --
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

  // --------------------------------------------------------- 2. wasm bytes --
  function getBytes(url) {
    if (hasGM) {
      return new Promise((res, rej) => GM_xmlhttpRequest({
        method: "GET", url, responseType: "arraybuffer",
        onload: (r) => (r.status === 200 ? res(r.response) : rej(new Error("HTTP " + r.status))),
        onerror: () => rej(new Error("network")), ontimeout: () => rej(new Error("timeout")),
      }));
    }
    return origFetch.call(self, url).then((r) => (r.ok ? r.arrayBuffer() : Promise.reject(new Error("HTTP " + r.status))));
  }

  const ready = (async () => {
    let firstErr = null;
    for (const url of CANDIDATES) {
      try {
        const buf = await getBytes(url);
        const mod = await WebAssembly.compile(buf);
        console.log("[raknet] patched engine compiled: " + url + " (" + buf.byteLength + " B)");
        return mod;
      } catch (e) { firstErr = firstErr || e; }
    }
    throw firstErr || new Error("no local module");
  })();
  ready.catch((e) => console.warn("[raknet] patched engine unavailable:", e && e.message,
    "- is tools/serve_wasm.py running on 127.0.0.1:8765?"));

  // -------------------------------------------------- 3. module substitution --
  const oInstantiate = WebAssembly.instantiate;
  const oCompileStreaming = WebAssembly.compileStreaming;

  WebAssembly.instantiate = function (m, imports) {
    return ready.then((mod) => oInstantiate.call(WebAssembly, mod, imports));
  };
  WebAssembly.instantiateStreaming = function (src, imports) {
    try { src && src.catch && src.catch(() => {}); } catch (e) {}
    return ready.then((mod) => oInstantiate.call(WebAssembly, mod, imports));
  };
  WebAssembly.compileStreaming = function () {
    return ready.then((mod) => mod);
  };

  console.log("[raknet] hook installed (GM_xmlhttpRequest=" + hasGM + ")");
})();
