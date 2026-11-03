// app.js — boot order, and nothing else.
//
//   1. frame logs in                  (cookie lives in the frame's jar)
//   2. join                           (frame -> join script)
//   3. ring points at the game server (one UDPSocket for the session)
//   4. engine loads                   (patched wasm, RakNet untouched)
//
// The window is the canvas and nothing else: progress goes to the console, and
// anything that actually stops the session goes to an alert. There is no status
// text on screen to read.
(() => {
  const say = (m) => console.log("[app] " + m);

  // One alert per distinct message. A failed join reports itself through several
  // channels, and a stack of identical dialogs is worse than the error.
  const shown = new Set();
  const fatal = (m) => {
    const text = String(m == null ? "Roblox ran into a problem." : m).slice(0, 500);
    console.error("[app] fatal: " + text);
    if (shown.has(text)) return;
    shown.add(text);
    window.alert(text);
  };

  // The engine narrates its loading screen here. A plain status is progress, so
  // the console keeps it; a failure is what the loading screen would have shown
  // as an error, and that is the case worth interrupting for.
  window.__onEngineLoadingStatus = (m) => {
    if (!m) return;
    if (typeof m === "string") return say(m);
    if (m.message) say(m.message);
    if (m.kind === "failure") fatal(m.message || "Roblox could not load this experience.");
  };

  // Benign engine errors happen during boot; they stay in the console.
  addEventListener("error", (e) => console.error("[app] window error:", e.message || e));
  addEventListener("unhandledrejection", (e) => console.error("[app] rejection:", e.reason));

  const params = new URLSearchParams(location.search);
  const placeId = params.get("placeId") || params.get("place");

  // Launching the installed app opens a bare origin, and the engine reads the
  // place id from the URL — without one it exits with "no startup place id".
  // So a bare launch redirects to the default place before anything else runs.
  if (!placeId) {
    location.replace(location.origin + "/?placeId=13569002876");
    return;
  }

  async function login(auth) {
    const who = await auth.whoami();
    if (who) return who;

    // No session in the frame's jar: show it and let the user log in once. The
    // partition persists, so this happens on a fresh install and never again.
    say("no session in the frame; showing the login page");
    return auth.interactiveLogin();
  }

  async function main() {
    const auth = new RobloxAuth({ onLog: say });
    self.__rbxAuth = auth;
    window.auth = auth;

    await auth.start();
    const who = await login(auth);
    say(`logged in as ${who.name}`);
    window.__whoami = who;

    say(`joining ${placeId}…`);
    const join = await auth.join(placeId);
    window.__join = join;
    // The engine asks gamejoin for a join of its own, in the website's shape;
    // that shape is refused on gated universes. Hand its request this reply
    // instead, so the join that happens is the one that just succeeded.
    if (self.__rbxNetSetJoin) self.__rbxNetSetJoin(join.reply);
    const peer = join.udmux || (join.connections && join.connections[0]);
    if (!peer) throw new Error("join script has no endpoint");

    await __rkRing.start({ host: peer.Address, port: peer.Port, script: join.script });
    say(`transport -> ${peer.Address}:${peer.Port}`);

    // Audio first. FMOD builds its sink during engine start; if the document has
    // not been touched yet, Chrome creates that AudioContext suspended and FMOD's
    // resume never lands, so the client loads in silence and never recovers. One
    // input before the engine runs makes the sink start cleanly.
    await firstInput();
    const splash = document.getElementById("splash");
    if (splash) splash.remove();

    // Only now: the ring has to be pointing somewhere before RakNet wakes up.
    const s = document.createElement("script");
    s.src = "/engine/WasmPlayer.js";
    s.onerror = () => fatal("Roblox could not load its engine.");
    document.body.appendChild(s);
  }

  // Resolves on the first real input, at once if the document has already been
  // activated (clicking during the join is enough), and otherwise after ten
  // seconds — a client that never gets clicked still has to load, even if that
  // leaves it silent.
  function firstInput() {
    const ua = navigator.userActivation;
    if (ua && ua.hasBeenActive) return Promise.resolve();
    return new Promise((resolve) => {
      const types = ["pointerdown", "keydown", "touchstart"];
      const done = () => {
        for (const type of types) removeEventListener(type, done, true);
        clearTimeout(timer);
        resolve();
      };
      const timer = setTimeout(done, 10000);
      for (const type of types) addEventListener(type, done, true);
    });
  }

  main().catch((e) => fatal(e && e.message ? e.message : e));
})();
