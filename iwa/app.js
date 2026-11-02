// app.js — boot order, and nothing else.
//
//   1. frame logs in                  (cookie lives in the frame's jar)
//   2. join                           (frame -> join script)
//   3. ring points at the game server (one UDPSocket for the session)
//   4. engine loads                   (patched wasm, RakNet untouched)
//
// Everything the engine needs from Roblox goes through rbx-net.js, which is
// loaded before this file so its fetch patch is already in place.
(() => {
  const hud = document.getElementById("hud") || { textContent: "", style: {} };
  const say = (m) => {
    hud.textContent = String(m).slice(0, 300);
    console.log("[app] " + m);
  };
  window.__onEngineLoadingStatus = say;
  // Errors go to the console, not the HUD: the engine raises benign ones during
  // boot and they would otherwise replace the line that says where the
  // transport landed.
  addEventListener("error", (e) => console.error("[app] window error:", e.message || e));
  addEventListener("unhandledrejection", (e) => console.error("[app] rejection:", e.reason));

  const params = new URLSearchParams(location.search);
  const placeId = params.get("placeId") || params.get("place") || "13569002876";

  async function login(auth) {
    const who = await auth.whoami();
    if (who) return who;

    // No session in the frame's jar: show it and let the user log in once. The
    // partition persists, so this happens on a fresh install and never again.
    say("log in to Roblox in the embedded page");
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
    const peer = join.udmux || (join.connections && join.connections[0]);
    if (!peer) throw new Error("join script has no endpoint");

    await __rkRing.start({ host: peer.Address, port: peer.Port, script: join.script });
    say(`transport -> ${peer.Address}:${peer.Port}`);

    // Only now: the ring has to be pointing somewhere before RakNet wakes up.
    const s = document.createElement("script");
    s.src = "/engine/WasmPlayer.js";
    s.onerror = () => say("engine failed to load");
    document.body.appendChild(s);
  }

  main().catch((e) => say("fatal: " + (e && e.message ? e.message : e)));
})();
