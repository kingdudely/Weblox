// Page-world hook (MAIN): force RakNet, then hand the engine our patched module.
(() => {
  if (globalThis.__rbxRaknetHook) return;
  globalThis.__rbxRaknetHook = 1;

  const W = document.documentElement.dataset.rbxWasmUrl;
  console.log("[raknet] module url " + W);

  // 1. transport flag
  const of = self.fetch;
  self.fetch = function (input, init) {
    const url = typeof input === "string" ? input : (input && input.url) || "";
    const p = of.call(self, input, init);
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

  // 2. patched module
  let mod = null;
  const ready = (async () => {
    const r = await of.call(self, W);
    if (!r.ok) throw new Error("HTTP " + r.status);
    const buf = await r.arrayBuffer();
    mod = await WebAssembly.compile(buf);
    console.log("[raknet] patched engine compiled (" + buf.byteLength + " B)");
    return mod;
  })();
  ready.catch((e) => console.warn("[raknet] patched engine unavailable:", e && e.message));

  const oI = WebAssembly.instantiate;
  WebAssembly.instantiate = function (m, imports) {
    return ready.then((mm) => oI.call(WebAssembly, mm, imports));
  };
  WebAssembly.instantiateStreaming = function (src, imports) {
    try { src && src.catch && src.catch(() => {}); } catch (e) {}
    return ready.then((mm) => oI.call(WebAssembly, mm, imports));
  };

  console.log("[raknet] hook installed");
})();
