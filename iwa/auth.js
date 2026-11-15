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
      this.frame = null;
      this.interceptor = null;
      this.seq = 0;
    }

    log(msg) {
      console.log("[auth] " + msg);
    }

    // ------------------------------------------------------------- frame ----
    /**
     * Create the frame and wait for roblox.com to finish loading. Its cookie
     * jar does not outlive the browser; the launch ticket is what logs it in.
     */
    async start(mount = document.body) {
      if (this.frame) return this.frame;

      const el = document.createElement("controlledframe");
      el.id = "rbx-auth";
      el.setAttribute("src", LOGIN_ORIGIN);
      // Hidden: the frame is a roblox.com context for the join, not the UI.
      el.style.cssText = "position:absolute;left:0;top:0;width:100%;height:100%;border:0;background:#111;display:none";

      const loaded = new Promise((resolve) => {
        const settle = (text) => {
          clearTimeout(timer);
          el.removeEventListener("loadstop", onStop);
          el.removeEventListener("loadabort", onAbort);
          this.log("frame " + text);
          resolve();
        };
        const onStop = () => settle("loaded");
        const onAbort = (e) => settle("aborted: " + (e.url || "unknown"));
        el.addEventListener("loadstop", onStop);
        el.addEventListener("loadabort", onAbort);
        const timer = setTimeout(() => settle("timed out"), 30000);
      });

      mount.appendChild(el);
      this.frame = el;
      this.installInterceptor();
      await loaded;
      return el;
    }

    /**
     * gamejoin and the auth ticket exchange want the native client's identity.
     * The scope matters: adding a custom header to every roblox.com URL turns
     * plain GETs into preflighted requests, and Roblox's CORS configs do not
     * allow the header.
     */
    installInterceptor() {
      if (this.interceptor || !this.frame) return this.interceptor;
      this.interceptor = this.frame.request.createWebRequestInterceptor({
        urlPatterns: ["*://gamejoin.roblox.com/*", "*://auth.roblox.com/*"],
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
     * Exchange the launch link's `gameinfo` ticket for a session, the same way
     * the native client does: the ticket is the credential, no cookie is sent,
     * and the reply sets .ROBLOSECURITY in the frame's jar. Single-use, so a
     * rejection is final: the launch link is the only credential.
     */
    async redeemTicket(ticket) {
      if (!ticket) throw new Error("no ticket");
      const attempt = (type, body) =>
        this.evalAsync(`
          const r = await fetch('https://auth.roblox.com/v1/authentication-ticket/redeem', {
            method: 'POST',
            credentials: 'include',
            headers: {
              'Content-Type': ${JSON.stringify(type)},
              'RBXAuthenticationNegotiation': '1'
            },
            body: ${JSON.stringify(body)}
          });
          return r.status;
        `);

      // Form-encoded is what the client sends; JSON is allowed too, so a
      // rejected shape still gets one chance at the other.
      const form = "authenticationTicket=" + encodeURIComponent(ticket);
      let status = await attempt("application/x-www-form-urlencoded", form);
      if (status !== 200) {
        const json = JSON.stringify({ authenticationTicket: ticket });
        status = await attempt("application/json", json);
      }
      if (status !== 200) throw new Error("redeem HTTP " + status);

      let who = await this.whoami();
      if (!who) {
        await sleep(300);
        who = await this.whoami();
      }
      if (!who) throw new Error("redeem did not log in");
      this.log(`redeemed launch ticket as ${who.name} (${who.id})`);
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
    async join(placeId, jobId) {
      if (!this.frame) throw new Error("frame not started");
      const place = Number(placeId) || 0;
      const requested = jobId ? JSON.stringify(String(jobId)) : "null";
      const result = await this.evalAsync(`
        const requestedJobId = ${requested};
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

        let reply = await post(requestedJobId);
        let tries = 0;
        while (reply.status === 0 && tries++ < 25) {
          await new Promise(r => setTimeout(r, 400));
          reply = await post(requestedJobId);
        }
        if (reply.status !== 2) {
          throw new Error('join status ' + reply.status + ' ' + (reply.message || ''));
        }

        let s = reply.joinScript;
        let carrier = reply;
        if (!s || typeof s !== 'object') {
          const again = await post(reply.jobId || requestedJobId);
          if (again.joinScript && typeof again.joinScript === 'object') {
            s = again.joinScript;
            carrier = again;
          }
        }
        if (!s || typeof s !== 'object') throw new Error('join script missing');

        return {
          // The engine asks for a join of its own; rbx-net answers that request
          // with this reply verbatim (see setJoinReply), so the whole body has
          // to survive the trip out of the frame.
          reply: carrier,
          jobId: reply.jobId || requestedJobId,
          tries,
          script: s
        };
      `, 30000);
      const join = result.script;
      const peer = (join.UdmuxEndpoints || [])[0] || (join.ServerConnections || [])[0] || null;
      this.log(`joined ${peer ? peer.Address + ":" + peer.Port : "without an endpoint"} after ${result.tries} tr${result.tries === 1 ? "y" : "ies"}`);
      return { ...result, peer };
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
        const send = (credentials) => fetch(c.url, {
          method: c.method || 'GET',
          credentials,
          headers: c.headers || {},
          body,
          redirect: c.redirect || 'follow'
        });
        let r;
        try {
          r = await send('include');
        } catch (e) {
          // A host that answers 'access-control-allow-origin: *' cannot be read
          // on a credentialed request — the browser rejects the pair outright,
          // which is what made every avatar, decal and animation missing. The
          // content CDN signs its URLs, so the cookie was never needed there.
          try {
            r = await send('omit');
          } catch (e2) {
            throw new Error(c.url + ': ' + e2.message);
          }
        }
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

  }

  window.RobloxAuth = RobloxAuth;
})();
