// app.js — boot order, and nothing else.
//
//   0. Chrome hands us the Play link   (roblox-player: / roblox:)
//   1. frame redeems its ticket       (one-time, from the link)
//   2. join                           (frame -> join script)
//   3. ring points at the game server (one UDPSocket for the session)
//   4. engine loads                   (patched wasm, RakNet untouched)
//
// The frame stays hidden: Chrome turns a roblox-player: navigation from inside
// it into about:blank, so games only start from a real browser. The window is
// the canvas; errors and disconnects end on one small screen. Everything the
// engine needs from Roblox goes through rbx-net.js, loaded before this file so
// its fetch patch is already in place.
(() => {
  const say = (m) => console.log("[app] " + m);

  // One screen for every ending: a failed launch, a leave, a dead socket. It is
  // plain DOM rather than a modal, which would block the renderer.
  let ended = false;
  const fatal = (m) => {
    const text = String(m == null ? "Roblox ran into a problem." : m).slice(0, 500);
    console.error("[app] fatal: " + text);
    showEnd(text, "Close this window, then press Play on roblox.com.");
  };

  function showEnd(title, detail) {
    if (ended) return;
    ended = true;
    title = title || "Disconnected";
    const screen = document.createElement("div");
    screen.style.cssText =
      "position:fixed;inset:0;z-index:2147483647;display:flex;flex-direction:column;" +
      "gap:18px;align-items:center;justify-content:center;background:#000;color:#fff;" +
      "font:16px/1.4 system-ui,sans-serif;text-align:center;padding:24px";
    const heading = document.createElement("div");
    heading.textContent = title;
    heading.style.cssText = "font-size:22px;font-weight:600";
    const note = document.createElement("div");
    note.textContent = detail || "Press Play on roblox.com to start another game.";
    note.style.cssText = "color:#aaa;max-width:38ch";
    const button = document.createElement("button");
    button.id = "end-close";
    button.textContent = "Close window";
    button.style.cssText =
      "font:inherit;padding:10px 20px;border-radius:8px;border:1px solid #444;" +
      "background:#1a1a1a;color:#fff;cursor:pointer";
    button.onclick = () => {
      window.close();
      setTimeout(() => {
        if (!window.closed) note.textContent = "Close this window from the title bar.";
      }, 300);
    };
    screen.append(heading, note, button);
    document.body.appendChild(screen);
  }
  window.__onSessionEnd = showEnd;

  window.__onEngineLoadingStatus = (m) => {
    if (!m) return;
    if (typeof m === "string") return say(m);
    if (m.message) say(m.message);
    if (m.kind === "failure") fatal(m.message || "Roblox could not load this experience.");
  };

  addEventListener("error", (e) => console.error("[app] window error:", e.message || e));
  addEventListener("unhandledrejection", (e) => console.error("[app] rejection:", e.reason));

  // ---------------------------------------------------------- launch ------
  // An installed app is opened from roblox.com's Play button, which emits the
  // same link the native launcher takes, and Chrome routes it here through the
  // manifest's protocol_handlers:
  //
  //   roblox-player:1+launchmode:play+gameinfo:<ticket>
  //     +placelauncherurl:assetgame.roblox.com/game/PlaceLauncher.ashx?…
  //        &placeId=1818&isPlayTogetherGame=false
  //     +launchtime:…+robloxTrackerId:…+robloxLocale:en_us+gameLocale:en_us
  //
  // The ids sit in `placelauncherurl`. `gameinfo` is a single-use login ticket:
  // roblox.com mints it from the browser's session when Play is pressed, and
  // redeeming it is how the native client ends up signed in. See auth.js.
  //
  // The other shape, `roblox://experiences/start?placeId=…`, is a real URL.
  //
  // Chrome substitutes the whole launch URI into `player=`, so the query holds
  // a URL inside a URL. Read it raw: URLSearchParams rewrites the link's "+"
  // separators into spaces, which would corrupt `gameinfo`.
  function rawParam(name) {
    const m = new RegExp("(?:^|[?&])" + name + "=([^&]*)").exec(location.search);
    if (!m) return "";
    try {
      return decodeURIComponent(m[1]);
    } catch (e) {
      return m[1];
    }
  }

  function parseLaunchText(text) {
    if (!text) return {};
    if (/^roblox-player:/i.test(text)) {
      // placeId arrives either literal or percent-encoded (`%3D`), depending
      // on whether Chrome passed the link through its handler or the wrapper.
      const place = text.match(/(?:[?&]|%26)placeId(?:=|%3D)(\d+)/i);
      const job = text.match(/(?:[?&]|%26)gameId(?:=|%3D)([0-9a-f-]{8,})/i);
      // Everything up to the next known field, not the next "+": a ticket may
      // contain one, and Chrome escapes the URI again, so the value may need
      // one more decode.
      const raw =
        (text.match(
          /\+gameinfo:([\s\S]*?)(?=\+(?:launchtime|placelauncherurl|browsertrackerid|robloxtrackerid|robloxlocale|gamelocale|launchexp|channel):|$)/i
        ) || [])[1] || "";
      let ticket = raw;
      if (/%[0-9a-f]{2}/i.test(raw)) {
        try {
          ticket = decodeURIComponent(raw);
        } catch (e) {
          // keep the undecoded value
        }
      }
      if (ticket === "0") ticket = "";
      return { placeId: place && place[1], jobId: job && job[1], ticket };
    }
    try {
      const url = new URL(text);
      return {
        placeId: url.searchParams.get("placeId") || url.searchParams.get("placeid"),
        jobId: url.searchParams.get("gameId") || url.searchParams.get("jobId"),
      };
    } catch (e) {
      return {};
    }
  }

  function launchTarget() {
    const payload = rawParam("player") || rawParam("deeplink");
    if (payload) {
      let text = payload;
      try {
        text = decodeURIComponent(payload);
      } catch (e) {
        // A stray "%" — the raw string still carries the ids.
      }
      return parseLaunchText(text);
    }
    // run_iwa.sh and manual testing pass the place directly.
    return { placeId: rawParam("placeId") || rawParam("place"), jobId: rawParam("jobId") };
  }

  const target = launchTarget();
  console.log(
    "[app] launch",
    target.placeId,
    target.jobId,
    target.ticket ? "with ticket" : "no ticket"
  );
  // Strip the one-time credential before anything awaits: it must not sit in
  // the URL, or in the window's history, any longer than the parse.
  try {
    history.replaceState(null, "", location.pathname);
  } catch (e) {
    // Opaque history entry; nothing to clean up.
  }

  if (!target.placeId || !target.ticket) {
    // The ticket is the only credential, so an icon launch, a stale link or a
    // roblox:// deeplink (which has no gameinfo) cannot start a session.
    fatal("Open roblox.com and press Play.");
    return;
  }

  async function main() {
    const auth = new RobloxAuth();
    self.__rbxAuth = auth;

    await auth.start();
    const who = await auth.redeemTicket(target.ticket);
    say(`logged in as ${who.name}`);
    window.__whoami = who;

    say(`joining ${target.placeId}${target.jobId ? " on " + target.jobId : ""}…`);
    const join = await auth.join(target.placeId, target.jobId);
    window.__join = join;
    // The engine asks gamejoin for a join of its own, in the website's shape;
    // that shape is refused on gated universes. Hand its request this reply
    // instead, so the join that happens is the one that just succeeded.
    if (self.__rbxNetSetJoin) self.__rbxNetSetJoin(join.reply);

    const peer = join.peer;
    if (!peer) throw new Error("join script has no endpoint");

    await __rkRing.start({ host: peer.Address, port: peer.Port, script: join.script });
    say(`transport -> ${peer.Address}:${peer.Port}`);

    // The loader reads its place from the query, not from the join reply. The
    // ticket was stripped above; this query carries only the place id.
    history.replaceState(null, "", "/?placeId=" + encodeURIComponent(target.placeId));

    // Only now: the ring has to be pointing somewhere before RakNet wakes up.
    const s = document.createElement("script");
    s.src = "/engine/WasmPlayer.js";
    s.onerror = () => fatal("Roblox could not load its engine.");
    document.body.appendChild(s);

    // A dead socket is the end of the session. Wait until the ring has been
    // running once before trusting "false".
    let live = false;
    setInterval(() => {
      if (!self.__rkRing) return;
      if (self.__rkRing.running) {
        live = true;
      } else if (live) {
        live = false;
        showEnd("Disconnected");
      }
    }, 1000);
  }

  main().catch((e) => fatal(e && e.message ? e.message : e));
})();
