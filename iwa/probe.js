// probe.js — the five questions that decide the architecture.
//
// Everything here is driven from outside (CDP) against an installed dev-mode
// IWA: the IWA's CSP has no localhost in connect-src, so the page cannot POST
// its results anywhere. Each step resolves to a plain JSON-able value.
(() => {
  const PANEL = document.getElementById("panel");
  const log = (cls, text) => {
    const line = document.createElement("div");
    line.className = cls;
    line.textContent = text;
    PANEL.appendChild(line);
    console.log("[probe] " + text);
  };
  window.__probeLog = log;

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // ---------------------------------------------------------------- step 0 ---
  // Does this origin actually have the capabilities we are betting on?
  function step_env() {
    const env = {
      origin: location.origin,
      isolated: self.crossOriginIsolated === true,
      sab: typeof SharedArrayBuffer === "function",
      udp: typeof UDPSocket,
      tcp: typeof TCPSocket,
      cfCtor: typeof HTMLControlledFrameElement,
      permissionsPolicy: (() => {
        try { return document.permissionsPolicy?.allowsFeature?.("controlled-frame"); } catch { return null; }
      })(),
      ua: navigator.userAgent,
    };
    log(env.isolated ? "ok" : "bad", `env: isolated=${env.isolated} udp=${env.udp} tcp=${env.tcp} cf=${env.cfCtor}`);
    return env;
  }

  // ---------------------------------------------------------------- step 1 ---
  // Element exists and exposes the webRequest interceptor.
  function step_element() {
    let el, err = null;
    try {
      el = document.createElement("controlledframe");
    } catch (e) {
      err = String(e);
    }
    const out = {
      created: !!el,
      ctor: el ? el.constructor.name : null,
      isCf: !!(el && self.HTMLControlledFrameElement && el instanceof HTMLControlledFrameElement),
      request: el ? typeof el.request : null,
      interceptor: null,
      error: err,
    };
    try {
      out.interceptor = typeof el.request.createWebRequestInterceptor;
    } catch (e) {
      out.interceptor = "throw: " + e;
    }
    log(out.isCf && out.interceptor === "function" ? "ok" : "bad",
      `element: created=${out.created} ctor=${out.ctor} request=${out.request} interceptor=${out.interceptor}`);
    return out;
  }

  // ---------------------------------------------------------------- frame ----
  let frame = null;
  function step_frame(src = "https://www.roblox.com/home", partition = "persist:roblox") {
    if (frame) frame.remove();
    const wrap = document.getElementById("cfwrap");
    wrap.style.display = "block";
    frame = document.createElement("controlledframe");
    frame.id = "cf";
    // partition must be set before src, or the frame gets the in-memory default.
    frame.setAttribute("partition", partition);
    frame.setAttribute("src", src);

    return new Promise((resolve) => {
      const done = (kind, detail) => {
        clearTimeout(timer);
        frame.removeEventListener("loadstop", onStop);
        frame.removeEventListener("loadabort", onAbort);
        frame.removeEventListener("contentload", onLoad);
        log(kind === "loadstop" ? "ok" : "bad", `frame: ${kind} ${detail}`);
        resolve({ event: kind, detail });
      };
      const onStop = () => done("loadstop", src);
      const onLoad = () => log("wait", "frame: contentload");
      const onAbort = (e) => done("loadabort", e.url || "");
      frame.addEventListener("loadstop", onStop);
      frame.addEventListener("contentload", onLoad);
      frame.addEventListener("loadabort", onAbort);
      wrap.appendChild(frame);
      const timer = setTimeout(() => done("timeout", src), 30000);
    });
  }

  // ---------------------------------------------------------------- step 2 ---
  // executeScript reaches into the roblox.com isolated world.
  async function step_exec(code = "location.href + ' | ' + document.title") {
    const t0 = performance.now();
    try {
      const r = await frame.executeScript({ code });
      const out = { ok: true, result: r, ms: Math.round(performance.now() - t0) };
      log("ok", `executeScript -> ${JSON.stringify(r)} (${out.ms} ms)`);
      return out;
    } catch (e) {
      log("bad", `executeScript threw: ${e}`);
      return { ok: false, error: String(e) };
    }
  }

  // ---------------------------------------------------------------- step 3 ---
  // Is the frame's storage agent logged in already?
  async function step_login() {
    // executeScript only carries JSON-able primitives back, so every step
    // returns a string rather than an object graph.
    const code = `(async () => {
      const out = { href: location.href, cookie: document.cookie.length };
      try {
        const r = await fetch('/mobileapi/userinfo', { credentials: 'include' });
        out.status = r.status;
        out.body = (await r.text()).slice(0, 300);
      } catch (e) { out.error = String(e); }
      return JSON.stringify(out);
    })()`;
    try {
      const r = await frame.executeScript({ code });
      const body = typeof r === "string" ? r : JSON.stringify(r);
      const loggedIn = /"Id"\s*:\s*[1-9]/.test(body) || /"id"\s*:\s*[1-9]/.test(body);
      log(loggedIn ? "ok" : "wait", `login: ${loggedIn ? "logged in" : "logged out"} :: ${body.slice(0, 240)}`);
      return { loggedIn, raw: r };
    } catch (e) {
      log("bad", `login probe threw: ${e}`);
      return { loggedIn: false, error: String(e) };
    }
  }

  // ---------------------------------------------------------------- step 4 ---
  // Write .ROBLOSECURITY into the frame's jar from inside its own origin.
  async function step_inject(cookieValue) {
    if (!cookieValue) return { ok: false, error: "no cookie value" };
    const code = `(async () => {
      document.cookie = ".ROBLOSECURITY=${String(cookieValue).replace(/["\\]/g, "")}; domain=.roblox.com; path=/; secure; samesite=lax";
      const r = await fetch('/mobileapi/userinfo', { credentials: 'include' });
      return r.status + ' ' + (await r.text()).slice(0, 200);
    })()`;
    const r = await frame.executeScript({ code });
    log("wait", `inject: ${String(r).slice(0, 200)}`);
    return { ok: true, result: r };
  }

  // ---------------------------------------------------------------- step 5 ---
  // Intercept the frame's traffic and rewrite the client identity.
  let interceptor = null;
  let seen = [];
  function step_interceptor() {
    if (!frame) return { ok: false, error: "no frame" };
    try {
      interceptor = frame.request.createWebRequestInterceptor({
        // Scope matters: a custom header on every roblox.com URL turns plain
        // GETs into preflighted requests, and the CORS configs only allow the
        // Roblox origins, not the header. Only gamejoin needs the rewrite.
        urlPatterns: ["*://gamejoin.roblox.com/*"],
        blocking: true,
        includeHeaders: "all",
      });
    } catch (e) {
      log("bad", `interceptor create threw: ${e}`);
      return { ok: false, error: String(e) };
    }

    const onBefore = (e) => {
      const url = e.url || e.request?.url;
      const headers = e.headers || e.request?.headers;
      const rec = { url, method: e.method || e.request?.method, headers: null, set: false };
      try {
        const h = new Headers(headers);
        rec.headers = { ua: h.get("user-agent"), requester: h.get("requester") };
        const patched = new Headers(headers);
        patched.set("User-Agent", "Roblox/WinInet");
        patched.set("Requester", "Client");
        e.setRequestHeaders(patched);
        rec.set = true;
      } catch (err) {
        rec.error = String(err);
      }
      seen.push(rec);
      log("wait", `beforesendheaders: ${rec.method || "?"} ${url} ua=${rec.headers?.ua} set=${rec.set}`);
    };
    const onComplete = (e) => {
      const url = e.url || e.request?.url;
      const status = e.response?.statusCode;
      if (url && url.includes("gamejoin")) log("wait", `completed: ${status} ${url}`);
    };

    interceptor.addEventListener("beforesendheaders", onBefore);
    interceptor.addEventListener("completed", onComplete);
    interceptor.addEventListener("erroroccurred", (e) => log("bad", "interceptor error: " + (e.error || e.url)));
    log("ok", "interceptor installed on *://*.roblox.com/*");
    return { ok: true, seen };
  }

  // ---------------------------------------------------------------- step 6 ---
  // The join, from inside the frame.
  //
  // Two things the native client taught us:
  //   * a fresh attempt id answers status 0 with a "JoinPlace=<id>;" placeholder
  //     and status 2 once the job exists, so retry while status is 0;
  //   * Game/Join.ashx answers 301 -> /request-error?code=404 now, so ignore
  //     joinScriptUrl: post again with the jobId and the join script comes back
  //     inline as an object.
  async function step_join(placeId) {
    const code = `(function(){
      window.__jr = { stage: 'start' };
      (async () => {
        const post = (jobId) => fetch('https://gamejoin.roblox.com/v1/join-game', {
          method: 'POST', credentials: 'include',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(Object.assign({
            placeId: ${Number(placeId) || 0}, isTeleport: false,
            gameJoinAttemptId: crypto.randomUUID(), browserTrackerId: 0,
            playSessionId: crypto.randomUUID(), eventId: crypto.randomUUID(),
            launchData: '', joinAttemptOrigin: 'PlayButton'
          }, jobId ? { jobId } : {}))
        }).then((r) => r.json());
        try {
          let reply = await post(null);
          let tries = 0;
          while (reply.status === 0 && tries++ < 20) {
            await new Promise((r) => setTimeout(r, 400));
            reply = await post(null);
          }
          if (reply.status !== 2) { window.__jr.stage = 'refused'; window.__jr.reply = reply; return; }
          let script = reply.joinScript;
          if (!script || typeof script !== 'object') {
            reply = await post(reply.jobId);
            script = reply.joinScript;
          }
          if (!script || typeof script !== 'object') {
            window.__jr.stage = 'noscript'; window.__jr.reply = { status: reply.status, jobId: reply.jobId }; return;
          }
          const udmux = (script.UdmuxEndpoints || [])[0] || null;
          window.__jr = {
            stage: 'done', tries,
            jobId: reply.jobId,
            machine: script.MachineAddress || null,
            serverPort: script.ServerPort || null,
            udmux,
            connections: script.ServerConnections || null,
            token: script.TokenValue || null,
            pepper: script.PepperId || null,
            clientTicket: (script.ClientTicket || '').slice(0, 24),
            userId: script.UserId || null,
            userName: script.UserName || null,
            keys: Object.keys(script).length
          };
        } catch (e) { window.__jr.stage = 'error'; window.__jr.err = String(e); }
      })();
      return 'started';
    })()`;
    log("wait", "join: dispatching");
    await frame.executeScript({ code });
    for (let i = 0; i < 160; i++) {
      await sleep(250);
      const poll = await frame.executeScript({ code: "JSON.stringify(window.__jr)" });
      const raw = Array.isArray(poll) ? poll[0] : poll;
      try {
        const st = JSON.parse(raw);
        if (["done", "error", "refused", "noscript"].includes(st.stage)) {
          log(st.stage === "done" ? "ok" : "bad", "join: " + raw.slice(0, 700));
          return st;
        }
      } catch (e) {}
    }
    log("bad", "join: timed out");
    return { stage: "timeout" };
  }

  window.__probe = {
    step_env, step_element, step_frame, step_exec, step_login,
    step_inject, step_interceptor, step_join,
    get frame() { return frame; },
    get seen() { return seen; },
  };
  log("wait", "probe loaded at " + location.origin);
})();
