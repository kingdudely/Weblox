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

  const live = new Set();
  const kicked = new WeakSet();

  const resumeAll = () => {
    for (const ctx of live) {
      if (!ctx.__hasRun()) continue;
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

  // Readout for the console: `__audio()` lists the live sinks and their states.
  window.__audio = () => [...live].map((c) => ({ state: c.state, rate: c.sampleRate, ran: c.__hasRun() }));
})();
