// boot.js — the globals the engine loader expects before it runs, plus the two
// things the browser forces on us: a Trusted Types policy and audio policy.

window.__wasmLoaderContext = {
  engineBase: location.origin + "/engine",
  server: "prod",
  channel: null,
  workerStubUrl: location.origin + "/stub.js",
};

// require-trusted-types-for 'script' rejects a plain string on script.src,
// new Worker(...) and importScripts(). A policy named "default" is applied to
// every sink automatically, so no call site — ours or the engine's — has to
// know about it. It asserts nothing about the URL: CSP already restricts scripts
// to 'self', and that is the check that holds.
if (typeof trustedTypes !== "undefined") {
  try {
    trustedTypes.createPolicy("default", {
      createScriptURL: (url) => url,
      createScript: (src) => src,
      createHTML: (html) => html,
    });
  } catch (e) {
    // A default policy already exists; nothing to add.
  }
}

// --------------------------------------------------------------- audio ------
// Two browser facts, both handled here because the engine gets them wrong.
//
// Chrome will not start an AudioContext before the page is interacted with, and
// the engine builds its own while booting. So every context is resumed on input
// (and retried every couple of seconds), which is what unlocks it.
//
// The engine also parks its mixer with mixerSuspend() at start-up and never
// lifts it: a suspended context never runs onaudioprocess, so the client is
// silent with a perfectly good graph. suspend() is therefore a no-op.
//
// FMOD's AudioWorklet output is built inside a resume() that never settles on
// this client, so it gets no sink at all; the ScriptProcessor output builds its
// node eagerly. Hiding AudioWorkletNode selects that one. See docs/IWA.md.
(() => {
  const Native = window.AudioContext || window.webkitAudioContext;
  if (!Native) return;

  window.AudioWorkletNode = undefined;

  const live = new Set();
  const sinks = [];

  const resume = () => {
    for (const ctx of live) if (ctx.state === "suspended") ctx.resume().catch(() => {});
  };

  class OpenAudioContext extends Native {
    constructor(...args) {
      super(...args);
      live.add(this);
      this.addEventListener("statechange", () => {
        if (this.state === "closed") live.delete(this);
      });
      this.suspend = () => Promise.resolve();
      resume();
    }
  }

  window.AudioContext = window.webkitAudioContext = OpenAudioContext;

  // A context created inside the gesture is allowed to start immediately. It
  // stays alive, which keeps the document unlocked for the engine's own context.
  const unlock = () => {
    try {
      new AudioContext();
    } catch (e) {
      console.warn("[audio] unlock failed:", e);
    }
  };

  // The engine's sink, held so the console can tell "no sink" from "silent".
  const nativeProcessor = AudioContext.prototype.createScriptProcessor;
  if (nativeProcessor) {
    AudioContext.prototype.createScriptProcessor = function (...args) {
      const node = nativeProcessor.apply(this, args);
      sinks.push(node);
      return node;
    };
  }

  for (const type of ["pointerdown", "keydown", "touchstart"]) {
    addEventListener(type, resume, { capture: true, passive: true });
    addEventListener(type, unlock, { capture: true, passive: true, once: true });
  }
  addEventListener("visibilitychange", resume);
  setInterval(resume, 2000);

  // `__audio()` — live contexts and the mixer's sink. `__audioPeak(ms)` — the
  // amplitude leaving that sink, which separates "silent mixer" from "muted".
  window.__audio = () => ({
    contexts: [...live].map((c) => ({ state: c.state, rate: c.sampleRate })),
    sinks: sinks.map((n) => ({ kind: n.constructor.name, state: n.context.state })),
  });

  window.__audioPeak = async (ms = 1000) => {
    const node = sinks[0];
    if (!node) return { error: "no sink" };
    const ctx = node.context;
    const analyser = ctx.createAnalyser();
    analyser.fftSize = 2048;
    node.connect(analyser);
    const buf = new Float32Array(analyser.fftSize);
    let peak = 0;
    const end = performance.now() + ms;
    while (performance.now() < end) {
      await new Promise((r) => setTimeout(r, 50));
      analyser.getFloatTimeDomainData(buf);
      for (const v of buf) peak = Math.max(peak, Math.abs(v));
    }
    node.disconnect(analyser);
    return { state: ctx.state, peak };
  };
})();

// ----------------------------------------------------------- hard stops -----
// The engine's loader catches its own start-up failures and only logs them, so a
// dead client would otherwise be a black window with one console line. Anything
// that stops start-up becomes one alert.
(() => {
  let alerted = false;
  const report = (text) => {
    if (alerted) return;
    alerted = true;
    window.alert("Roblox failed to start.\n\n" + String(text).split("\n")[0].slice(0, 300));
  };

  const nativeError = console.error.bind(console);
  console.error = (...args) => {
    nativeError(...args);
    const text = args.map((a) => (a && a.message) || String(a)).join(" ");
    if (alerted) return;
    if (/startup failed|could not allocate memory|Invalid atomic access index/i.test(text)) report(text);
  };

  addEventListener("error", (e) => {
    if (self.Module) return;
    report((e && e.message) || e);
  });
})();
