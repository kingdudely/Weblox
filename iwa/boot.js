// boot.js — the globals the engine loader expects before it runs, plus the two
// things the IWA policy forces on us.
window.__wasmLoaderContext = {
  engineBase: location.origin + "/engine",
  server: "prod",
  channel: null,
  workerStubUrl: location.origin + "/stub.js",
};

// ---------------------------------------------------------------------------
// require-trusted-types-for 'script': any script URL, worker URL or
// importScripts string has to come from a policy. One permissive policy is
// enough; it asserts nothing about the URL, which is fine because every one is
// same-origin or a constant in this app.
window.__tt = (() => {
  const passthrough = { url: (u) => u, html: (h) => h };
  if (typeof trustedTypes === "undefined") return passthrough;
  let policy;
  try {
    policy = trustedTypes.createPolicy("weblox", {
      createScriptURL: (u) => u,
      createHTML: (h) => h,
    });
  } catch (e) {
    return passthrough;
  }
  return { url: (u) => policy.createScriptURL(u), html: (h) => policy.createHTML(h) };
})();

// The engine assigns script.src directly, so the policy has to sit on the
// property rather than at our call sites.
if (typeof HTMLScriptElement !== "undefined") {
  const desc = Object.getOwnPropertyDescriptor(HTMLScriptElement.prototype, "src");
  Object.defineProperty(HTMLScriptElement.prototype, "src", {
    configurable: true,
    enumerable: desc.enumerable,
    get: desc.get,
    set(v) {
      desc.set.call(this, __tt.url(String(v)));
    },
  });
}

// Workers are created by URL too.
if (typeof Worker !== "undefined") {
  const RealWorker = Worker;
  const Wrapped = function (url, opts) {
    return new RealWorker(__tt.url(String(url)), opts);
  };
  Wrapped.prototype = RealWorker.prototype;
  Object.setPrototypeOf(Wrapped, RealWorker);
  self.Worker = Wrapped;
}

// The probe is a diagnostic, not part of the app: only on ?probe.
if (location.search.includes("probe")) {
  const s = document.createElement("script");
  s.src = __tt.url("/probe.js");
  document.head.appendChild(s);
}
