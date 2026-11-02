// auth.js — the join, done by a real roblox.com context.
//
// <controlledframe> gives the page a navigable context on www.roblox.com with
// its own cookie jar. From there the join is same-site, so the browser attaches
// .ROBLOSECURITY itself and CORS is Roblox's own policy. The only thing we have
// to change on the wire is the client identity, because gamejoin hands out a
// session that matches the client that asked for it: a browser user agent gets
// status 12 "Unable to join Game".
//
// Two facts from the native client that this module encodes:
//   * gamejoin is asynchronous for a fresh attempt: status 0 with a
//     "JoinPlace=<id>;" placeholder, status 2 once the job exists. Retry.
//   * Game/Join.ashx now answers 301 -> /request-error?code=404. Posting the
//     join again *with the returned jobId* returns the script inline instead.
(() => {
  const LOGIN_ORIGIN = "https://www.roblox.com/home";
  const GAMEJOIN = "https://gamejoin.roblox.com/v1/join-game";
  const CLIENT_UA = "Roblox/WinInet";
  const POLL_MS = 50;

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  class RobloxAuth {
    constructor(opts = {}) {
      this.partition = opts.partition || "persist:roblox";
      this.onLog = opts.onLog || (() => {});
      this.frame = null;
      this.interceptor = null;
      this.seq = 0;
      this.loaded = false;
    }

    log(msg) {
      this.onLog(String(msg));
      console.log("[auth] " + msg);
    }

    // ------------------------------------------------------------- frame ----
    /**
     * Create the frame and wait for roblox.com to finish loading. The
     * partition must be set before src or the frame gets an in-memory jar that
     * dies with the element.
     */
    async start(mount = document.body) {
      if (this.frame) return this.frame;

      const el = document.createElement("controlledframe");
      el.id = "rbx-auth";
      el.setAttribute("partition", this.partition);
      el.setAttribute("src", LOGIN_ORIGIN);
      // Hidden until an interactive login is actually needed.
      el.style.cssText = "position:absolute;left:0;top:0;width:100%;height:100%;border:0;background:#111;display:none";

      const loaded = new Promise((resolve) => {
        const done = (ok, detail) => {
          clearTimeout(timer);
          el.removeEventListener("loadstop", onStop);
          el.removeEventListener("loadabort", onAbort);
          this.loaded = ok;
          this.log(`frame ${ok ? "loaded" : "aborted"}: ${detail}`);
          resolve(ok);
        };
        const onStop = () => done(true, LOGIN_ORIGIN);
        const onAbort = (e) => done(false, e.url || "unknown");
        el.addEventListener("loadstop", onStop);
        el.addEventListener("loadabort", onAbort);
        const timer = setTimeout(() => done(false, "timeout"), 30000);
      });

      mount.appendChild(el);
      this.frame = el;
      this.installInterceptor();
      await loaded;
      return el;
    }

    /**
     * gamejoin is the only host that needs the identity rewrite, and the scope
     * matters: adding a custom header to every roblox.com URL turns plain GETs
     * into preflighted requests, and Roblox's CORS configs do not allow the
     * header.
     */
    installInterceptor() {
      if (this.interceptor || !this.frame) return this.interceptor;
      this.interceptor = this.frame.request.createWebRequestInterceptor({
        urlPatterns: ["*://gamejoin.roblox.com/*"],
        blocking: true,
        includeHeaders: "all",
      });
      this.interceptor.addEventListener("beforesendheaders", (event) => {
        try {
          const original = event.headers || event.request?.headers;
          const headers = new Headers(original);
          headers.set("User-Agent", CLIENT_UA);
          event.setRequestHeaders(headers);
        } catch (e) {
          this.log("interceptor: " + e);
        }
      });
      this.interceptor.addEventListener("erroroccurred", (e) => this.log("request failed: " + (e.error || e.url)));
      return this.interceptor;
    }

    // ------------------------------------------------------- executeScript ---
    /**
     * executeScript returns the value of the last *statement* and does not
     * await promises. So an async body is started and its result stashed in the
     * frame's isolated world, which persists between calls, then polled.
     */
    async evalAsync(body, timeoutMs = 20000) {
      if (!this.frame) throw new Error("frame not started");
      const id = ++this.seq;
      const boot = `window.__rbx = window.__rbx || {};
        window.__rbx[${id}] = { done: false };
        (async () => {
          try { window.__rbx[${id}] = { done: true, ok: true, value: await (async () => { ${body} })() }; }
          catch (e) { window.__rbx[${id}] = { done: true, ok: false, error: String(e) }; }
        })();
        'go'`;
      await this.frame.executeScript({ code: boot });

      const deadline = performance.now() + timeoutMs;
      while (performance.now() < deadline) {
        await sleep(POLL_MS);
        const res = await this.frame.executeScript({
          code: `JSON.stringify(window.__rbx && window.__rbx[${id}])`,
        });
        const raw = Array.isArray(res) ? res[0] : res;
        if (!raw || raw === "undefined") continue;
        let env;
        try {
          env = JSON.parse(raw);
        } catch (e) {
          continue;
        }
        if (!env || !env.done) continue;
        if (!env.ok) throw new Error(env.error);
        return env.value;
      }
      throw new Error("evalAsync timed out after " + timeoutMs + "ms");
    }

    async evalSync(code) {
      const res = await this.frame.executeScript({ code });
      return Array.isArray(res) ? res[0] : res;
    }

    // ------------------------------------------------------------- login ----
    /** Is the frame's storage agent logged in? */
    async whoami() {
      const out = await this.evalAsync(`
        const r = await fetch('https://users.roblox.com/v1/users/authenticated', { credentials: 'include' });
        if (r.status !== 200) return null;
        const u = await r.json();
        return { id: u.id, name: u.name, displayName: u.displayName };
      `);
      return out || null;
    }

    /**
     * Write the cookie into the frame's jar from inside its own origin. Chrome
     * allows overwriting an HttpOnly cookie from script; only reading is
     * blocked. Used when the caller already has a .ROBLOSECURITY, so the first
     * run does not need a visible login.
     */
    async injectCookie(value) {
      if (!value) throw new Error("empty cookie");
      const literal = JSON.stringify(".ROBLOSECURITY=" + value + "; domain=.roblox.com; path=/; secure; samesite=lax");
      await this.evalAsync(`document.cookie = ${literal}; await new Promise(r => setTimeout(r, 50)); return true;`);
      const who = await this.whoami();
      if (!who) throw new Error("cookie rejected");
      this.log(`logged in as ${who.name} (${who.id})`);
      return who;
    }

    // -------------------------------------------------------------- join ----
    /**
     * Join a place and return its join script object.
     *
     * Content-Type is sent explicitly: the preflight for `application/json` is
     * allowed, and the interceptor's User-Agent rewrite rides on the same
     * request.
     */
    async join(placeId) {
      if (!this.frame) throw new Error("frame not started");
      const place = Number(placeId) || 0;
      const script = await this.evalAsync(`
        const post = (jobId) => fetch('${GAMEJOIN}', {
          method: 'POST',
          credentials: 'include',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(Object.assign({
            placeId: ${place},
            isTeleport: false,
            gameJoinAttemptId: crypto.randomUUID(),
            browserTrackerId: 0,
            playSessionId: crypto.randomUUID(),
            eventId: crypto.randomUUID(),
            launchData: '',
            joinAttemptOrigin: 'PlayButton'
          }, jobId ? { jobId } : {}))
        }).then(r => r.json());

        let reply = await post(null);
        let tries = 0;
        while (reply.status === 0 && tries++ < 25) {
          await new Promise(r => setTimeout(r, 400));
          reply = await post(null);
        }
        if (reply.status !== 2) {
          throw new Error('join status ' + reply.status + ' ' + (reply.message || ''));
        }

        let s = reply.joinScript;
        if (!s || typeof s !== 'object') {
          const again = await post(reply.jobId);
          if (again.joinScript && typeof again.joinScript === 'object') s = again.joinScript;
        }
        if (!s || typeof s !== 'object') throw new Error('join script missing');

        return {
          jobId: reply.jobId,
          tries,
          userId: s.UserId,
          userName: s.UserName,
          machine: s.MachineAddress,
          serverPort: s.ServerPort,
          udmux: (s.UdmuxEndpoints || [])[0] || null,
          connections: s.ServerConnections || null,
          token: s.TokenValue,
          pepper: s.PepperId,
          clientTicket: s.ClientTicket,
          gameId: s.GameId,
          placeId: s.PlaceId,
          script: s
        };
      `, 30000);
      this.log(`joined ${script.udmux?.Address}:${script.udmux?.Port} via ${script.tries} retr${script.tries === 1 ? "y" : "ies"}`);
      return script;
    }

    // ------------------------------------------------------------ bridge ----
    /**
     * One authenticated roblox.com request from inside the frame.
     *
     * The engine runs on isolated-app://, which is cross-origin to everything
     * Roblox and cannot attach a cookie. Every API call it makes is routed here
     * instead. Bodies cross as text or base64 so the value stays JSON-able.
     */
    async request(url, init = {}) {
      const cfg = JSON.stringify({ url, ...init });
      return this.evalAsync(`
        const c = ${cfg};
        let body = c.body != null ? c.body : undefined;
        if (c.bodyB64) {
          const bin = atob(c.bodyB64);
          body = new Uint8Array(bin.length);
          for (let i = 0; i < bin.length; i++) body[i] = bin.charCodeAt(i);
        }
        const r = await fetch(c.url, {
          method: c.method || 'GET',
          credentials: 'include',
          headers: c.headers || {},
          body,
          redirect: c.redirect || 'follow'
        });
        const buf = await r.arrayBuffer();
        const bytes = new Uint8Array(buf);
        let b64 = '';
        const CH = 0x8000;
        for (let i = 0; i < bytes.length; i += CH) {
          b64 += String.fromCharCode.apply(null, bytes.subarray(i, i + CH));
        }
        return {
          status: r.status,
          url: r.url,
          headers: Object.fromEntries(r.headers.entries()),
          body: btoa(b64)
        };
      `, 60000);
    }

    /** Show or hide the frame. It has to be visible for an interactive login. */
    visible(on) {
      if (this.frame) this.frame.style.display = on ? "block" : "none";
    }

    /** Show the frame on top and wait until the user has logged in. */
    async interactiveLogin(timeoutMs = 10 * 60 * 1000) {
      this.visible(true);
      const deadline = performance.now() + timeoutMs;
      while (performance.now() < deadline) {
        const who = await this.whoami().catch(() => null);
        if (who) {
          this.visible(false);
          return who;
        }
        await sleep(2000);
      }
      throw new Error("login timed out");
    }

    async logout() {
      if (this.frame) await this.frame.clearData();
      this.log("frame data cleared");
    }
  }

  window.RobloxAuth = RobloxAuth;
})();
