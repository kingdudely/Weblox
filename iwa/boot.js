// boot.js — the globals the engine loader expects before it runs, plus the one
// line the IWA policy forces on us.

window.__wasmLoaderContext = {
  engineBase: location.origin + "/engine",
  server: "prod",
  channel: null,
  workerStubUrl: location.origin + "/stub.js",
};

// require-trusted-types-for 'script' rejects a plain string on script.src,
// new Worker(...) and importScripts(). A policy named "default" is applied to
// every sink automatically, so no call site — ours or the engine's — has to
// know about it.
//
// It asserts nothing about the URL, which is the honest position: CSP already
// restricts scripts to 'self', and that is the check that actually holds.
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

// --------------------------------------------------------------- hard stops ---
// The engine's loader catches its own start-up failures and only logs them, so a
// dead client would otherwise be a black window with one console line. Anything
// that stops start-up is escalated to an alert, once.
(() => {
  const nativeError = console.error.bind(console);
  let alerted = false;
  console.error = (...args) => {
    nativeError(...args);
    if (alerted) return;
    const text = args.map((a) => (a && a.message) || String(a)).join(" ");
    if (!/startup failed|could not allocate memory|Invalid atomic access index/i.test(text)) return;
    alerted = true;
    window.alert("Roblox failed to start.\n\n" + text.split("\n")[0].slice(0, 300));
  };
})();

// ------------------------------------------------------------------- audio ---
// Chrome will not start an AudioContext until the page has been interacted
// with, and FMOD needs more than a resume: its documented browser workaround is
// mixerSuspend() followed by mixerResume() inside the gesture. A bare resume()
// while the first one is still pending does nothing at all, which is exactly
// what the engine's own hook does — it calls resume_audio() and discards the
// result — so the client loads in silence and never recovers.
//
// Two parts here, both on the context FMOD builds:
//   * ignore a suspend() that arrives before the context has run once. Chrome
//     latches that state: every later resume() rejects with InvalidStateError
//     and the sink can never come up.
//   * on the first input, do the suspend/resume pair for every live context, and
//     keep resuming anything that is left suspended.
(() => {
  const Native = window.AudioContext || window.webkitAudioContext;
  if (!Native) return;

  // FMOD builds its AudioWorklet sink *inside* `mContext.resume().then(...)`, so
  // no sink exists until that promise settles — and on this client it never
  // settles, which leaves the mixer with nowhere to write. The ScriptProcessor
  // output builds its node eagerly at start-up, so the mixer is wired whether or
  // not the context has been unlocked yet. Slower (main-thread mixing), but it
  // is the difference between a silent client and an audible one. Revisit when
  // the worklet path can be shown to resume — see docs/IWA.md.
  window.AudioWorkletNode = undefined;

  const live = new Set();
  const kicked = new WeakSet();

  const resumeAll = () => {
    for (const ctx of live) {
      // Never suspend a context that has not run yet: that is the latch. A plain
      // resume is the only thing it can accept, and it also releases the resume
      // the engine issued at start-up.
      if (!ctx.__hasRun()) {
        ctx.resume().catch(() => {});
        continue;
      }
      if (!kicked.has(ctx)) {
        kicked.add(ctx);
        ctx.suspend().then(() => ctx.resume()).catch(() => {});
      } else if (ctx.state === "suspended") {
        ctx.resume().catch(() => {});
      }
    }
  };

  class ResumableAudioContext extends Native {
    constructor(...args) {
      super(...args);
      live.add(this);

      let ran = this.state === "running";
      const suspend = this.suspend.bind(this);

      this.addEventListener("statechange", () => {
        if (this.state === "running") ran = true;
        if (this.state === "closed") live.delete(this);
      });

      this.suspend = () => (ran ? suspend() : Promise.resolve());
      this.__hasRun = () => ran;

      resumeAll();
    }
  }

  window.AudioContext = ResumableAudioContext;
  window.webkitAudioContext = ResumableAudioContext;

  // A context created inside the gesture is allowed to start, and keeping it
  // alive keeps the document's audio unlocked when the engine opens its sink a
  // few seconds later.
  const unlock = () => {
    try {
      const ctx = new AudioContext();
      ctx.resume().catch(() => {});
      console.log("[audio] unlock " + ctx.state);
    } catch (e) {
      console.warn("[audio] unlock failed:", e);
    }
  };

  for (const type of ["pointerdown", "keydown", "touchstart"]) {
    addEventListener(type, resumeAll, { capture: true, passive: true });
    addEventListener(type, unlock, { capture: true, passive: true, once: true });
  }
  addEventListener("visibilitychange", resumeAll);

  // A context created before the first input stays suspended until something
  // retries it. Polling is cheap and means the click that comes later is not
  // missed because the engine loaded after it.
  setInterval(() => {
    for (const ctx of live) {
      if (ctx.state === "suspended") return resumeAll();
    }
  }, 2000);

  // ---- diagnostics -----------------------------------------------------------
  // The mixer's sink is either an AudioWorkletNode (fast path, needs
  // crossOriginIsolated) or a ScriptProcessorNode (fallback). Hold whichever
  // appears so `__audio()` can say whether there is one at all, and
  // `__audioPeak()` can measure what comes out of it.
  const sinks = [];
  const NativeNode = window.AudioWorkletNode;
  if (NativeNode) {
    window.AudioWorkletNode = new Proxy(NativeNode, {
      construct(target, args) {
        const node = Reflect.construct(target, args);
        sinks.push(node);
        return node;
      },
    });
  }
  const nativeProcessor = AudioContext.prototype.createScriptProcessor;
  if (nativeProcessor) {
    AudioContext.prototype.createScriptProcessor = function (...args) {
      const node = nativeProcessor.apply(this, args);
      sinks.push(node);
      return node;
    };
  }

  // `__audio()` — one entry per live context and whether a sink exists.
  window.__audio = () => ({
    contexts: [...live].map((c) => ({ state: c.state, rate: c.sampleRate, ran: c.__hasRun() })),
    sinks: sinks.map((n) => ({ kind: n.constructor.name, state: n.context.state })),
  });

  // `__audioPeak(ms)` — peak amplitude leaving the first sink. Zero while the
  // game is quiet, or while the context is suspended; that is the difference
  // between "no sink" and "no sound playing".
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
