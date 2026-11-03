// app.js — boot order, and nothing else.
//
//   1. frame logs in                  (cookie lives in the frame's jar)
//   2. join                           (frame -> join script)
//   3. ring points at the game server (one UDPSocket for the session)
//   4. engine loads                   (patched wasm, RakNet untouched)
//
// The window is the canvas and nothing else: progress goes to the console, and
// anything that actually stops the session becomes an alert. Everything the
// engine needs from Roblox goes through rbx-net.js, which is loaded before this
// file so its fetch patch is already in place.
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

  // The engine narrates its loading screen here. A status is progress, so the
  // console keeps it; a failure is what the loading screen would have shown as
  // an error, and that is the case worth interrupting for.
  window.__onEngineLoadingStatus = (m) => {
    if (!m) return;
    if (typeof m === "string") return say(m);
    if (m.message) say(m.message);
    if (m.kind === "failure") fatal(m.message || "Roblox could not load this experience.");
  };

  addEventListener("error", (e) => console.error("[app] window error:", e.message || e));
  addEventListener("unhandledrejection", (e) => console.error("[app] rejection:", e.reason));

  const params = new URLSearchParams(location.search);
  const placeId = params.get("placeId") || params.get("place");

  // Launching the installed app opens a bare origin, and the engine reads the
  // place id from the URL — without one it exits with "no startup place id".
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

    // Only now: the ring has to be pointing somewhere before RakNet wakes up.
    const s = document.createElement("script");
    s.src = "/engine/WasmPlayer.js";
    s.onerror = () => fatal("Roblox could not load its engine.");
    document.body.appendChild(s);
  }

  main().catch((e) => fatal(e && e.message ? e.message : e));
})();
