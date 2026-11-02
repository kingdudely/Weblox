// MAIN-world hook, document_start: force the engine onto RakNet.
// The module itself is swapped at the network layer by rules.json, so this
// works no matter which context (page or worker) instantiates it.
(() => {
  if (globalThis.__rbxRaknetHook) return;
  globalThis.__rbxRaknetHook = 1;

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

  // Same hook inside the engine worker, which is where the module is fetched.
  const makeWorker = self.Worker;
  self.Worker = function (url, opts) {
    const w = new makeWorker(url, opts);
    return w;
  };
  console.log("[raknet] hook installed");
})();
