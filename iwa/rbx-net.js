// rbx-net.js — every roblox.com call, answered from the authenticated frame.
//
// The engine runs on isolated-app://, which is cross-origin to everything
// Roblox: it cannot attach .ROBLOSECURITY and Roblox's CORS configs do not
// list that origin. The controlled frame is a real www.roblox.com context with
// the cookie in its jar, so this file redirects the hosts below to it and
// rebuilds a normal Response. Assets on rbxcdn are not in the list: they are
// public and CORS-open, so they keep going straight out.
//
// Workers cannot reach the frame, so they reach the page over a
// BroadcastChannel instead of borrowing the engine's own postMessage protocol.
(() => {
  if (self.__rbxNet) return;
  self.__rbxNet = 1;

  const HOSTS = new Set([
    "gamejoin.roblox.com", "assetgame.roblox.com", "www.roblox.com",
    "apis.roblox.com",
    "assetdelivery.roblox.com", "users.roblox.com", "economy.roblox.com",
    "games.roblox.com", "thumbnails.roblox.com", "locale.roblox.com",
    "avatar.roblox.com", "accountsettings.roblox.com", "auth.roblox.com",
    "tracing.roblox.com", "ecsv2.roblox.com", "ephemeralcounters.api.roblox.com",
    "gameinternationalization.roblox.com", "develop.roblox.com", "groups.roblox.com",
    "presence.roblox.com", "voice.roblox.com", "chat.roblox.com",
    "friends.roblox.com", "badges.roblox.com", "inventory.roblox.com",
    "catalog.roblox.com", "accountinformation.roblox.com", "trades.roblox.com",
    "metrics.roblox.com", "realtime.roblox.com", "notifications.roblox.com",
  ]);

  const base = (typeof location !== "undefined" && location.origin) || "https://localhost";
  const isPage = typeof document !== "undefined";

  function hostOf(url) {
    try {
      return new URL(url, base).hostname;
    } catch (e) {
      return "";
    }
  }

  const isRoblox = (url) => HOSTS.has(hostOf(url));

  // ------------------------------------------------------------ transport ---
  const CHANNEL_NAME = "rbxnet";
  let channel = null;
  if (typeof BroadcastChannel !== "undefined") {
    try {
      channel = new BroadcastChannel(CHANNEL_NAME);
    } catch (e) {}
  }

  let seq = 0;
  const waiting = new Map(); // id -> resolve

  if (channel) {
    channel.onmessage = (ev) => {
      const msg = ev.data;
      if (!msg) return;
      if (msg.kind === "req" && isPage && self.__rbxAuth) {
        self.__rbxAuth
          .request(msg.url, msg.init)
          .then((res) => channel.postMessage({ kind: "res", id: msg.id, res }))
          .catch((e) => channel.postMessage({ kind: "res", id: msg.id, error: String(e) }));
      } else if (msg.kind === "res") {
        const resolve = waiting.get(msg.id);
        if (resolve) {
          waiting.delete(msg.id);
          resolve(msg);
        }
      }
    };
  }

  function b64ToBytes(b64) {
    const bin = atob(b64);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  function bytesToB64(bytes) {
    let bin = "";
    const CH = 0x8000;
    for (let i = 0; i < bytes.length; i += CH) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + CH));
    }
    return btoa(bin);
  }

  /** Normalise a fetch init into something structured-cloneable. */
  async function encodeInit(init = {}) {
    const out = {
      method: init.method || "GET",
      headers: {},
      redirect: init.redirect || "follow",
    };
    const h = new Headers(init.headers || {});
    for (const [k, v] of h) out.headers[k] = v;
    let body = init.body;
    if (body == null) return out;
    if (typeof body === "string") {
      out.body = body;
    } else if (body instanceof URLSearchParams) {
      out.body = body.toString();
      out.headers["content-type"] = out.headers["content-type"] || "application/x-www-form-urlencoded;charset=UTF-8";
    } else if (body instanceof Blob) {
      out.body = await body.text();
    } else if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) {
      const bytes = body instanceof ArrayBuffer ? new Uint8Array(body) : new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
      out.bodyB64 = bytesToB64(bytes);
      out.body = "";
    } else {
      out.body = String(body);
    }
    return out;
  }

  function viaChannel(url, init) {
    if (!channel) return Promise.reject(new Error("no BroadcastChannel for " + url));
    const id = ++seq;
    return new Promise((resolve, reject) => {
      waiting.set(id, (msg) => {
        if (msg.error) reject(new Error(msg.error));
        else resolve(msg.res);
      });
      channel.postMessage({ kind: "req", id, url, init });
    });
  }

  const recent = [];
  self.__rbxNetLog = recent;

  async function rbxRequest(url, init) {
    const encoded = await encodeInit(init);
    let res;
    try {
      res = isPage && self.__rbxAuth ? await self.__rbxAuth.request(url, encoded) : await viaChannel(url, encoded);
    } catch (e) {
      recent.push({ url, method: encoded.method, error: String(e) });
      if (recent.length > 200) recent.shift();
      throw e;
    }
    recent.push({ url, method: encoded.method, status: res.status });
    if (recent.length > 200) recent.shift();
    return res;
  }

  function toResponse(res) {
    const headers = new Headers(res.headers || {});
    const bytes = res.body ? b64ToBytes(res.body) : new Uint8Array(0);
    const nullBody = bytes.length === 0 || res.status === 204 || res.status === 205 || res.status === 304;
    return new Response(nullBody ? null : bytes, { status: res.status, statusText: "", headers });
  }

  // --------------------------------------------------------------- patch ----
  // clientsettings is public and CORS-open, so it goes straight out. It is
  // also where the transport flag lives: RakNet only gets used if the engine is
  // told not to pick RbxTransport.
  const SETTINGS_RE = /^https:\/\/clientsettings(cdn)?\.roblox\.com\/v2\/settings\/application\/WebClientApp/;

  async function settingsOverride(url, init) {
    const r = await origFetch.call(self, url, { ...(init || {}), credentials: "omit" });
    let doc;
    try {
      doc = await r.json();
    } catch (e) {
      return r;
    }
    const app = doc.applicationSettings;
    if (app) {
      app.FFlagUseRbxTransportClient3 = "False";
      app.FFlagUseRbxTransportServer = "False";
    }
    console.log("[rbxnet] settings override -> " + JSON.stringify({
      raknet: app && app.FFlagUseRbxTransportClient3,
      status: r.status,
    }));
    return new Response(JSON.stringify(doc), {
      status: r.status,
      headers: { "content-type": "application/json" },
    });
  }

  const origFetch = self.fetch;
  self.fetch = function (input, init) {
    const url = typeof input === "string" ? input : input && input.url;
    if (url && SETTINGS_RE.test(new URL(url, base).href)) return settingsOverride(new URL(url, base).href, init);
    if (!url || !isRoblox(url)) return origFetch.call(self, input, init);
    const opts = init || (input && input.url && !(input instanceof Request) ? undefined : input);
    // A Request body has to be read before it can cross the channel.
    const run = input instanceof Request && !init ? input.text().then((text) => ({ ...(opts || {}), body: text })) : Promise.resolve(opts || {});
    return run.then((o) => rbxRequest(new URL(url, base).href, o)).then(toResponse);
  };

  // Emscripten's fetch backend drives XMLHttpRequest, so a small shim that
  // resolves through the same path keeps it working without a full XHR.
  const OrigXHR = self.XMLHttpRequest;
  if (OrigXHR) {
    class FrameXHR extends EventTarget {
      constructor() {
        super();
        this.readyState = 0;
        this.status = 0;
        this.statusText = "";
        this.response = null;
        this.responseText = "";
        this.responseType = "";
        this.withCredentials = false;
        this.timeout = 0;
        this._headers = {};
        this._resHeaders = new Headers();
        this._aborted = false;
        this.onload = null;
        this.onerror = null;
        this.onreadystatechange = null;
        this.ontimeout = null;
        this.onprogress = null;
      }

      open(method, url) {
        this._method = method;
        this._url = new URL(url, base).href;
        this._robox = isRoblox(this._url);
        if (!this._robox) {
          this._real = new OrigXHR();
          const real = this._real;
          real.onload = () => this._syncFromReal();
          real.onerror = () => this._fail();
          real.open(...arguments);
          return;
        }
        this.readyState = 1;
        this._fire("readystatechange");
      }

      setRequestHeader(k, v) {
        if (this._real) return this._real.setRequestHeader(k, v);
        this._headers[k] = v;
      }

      getResponseHeader(k) {
        if (this._real) return this._real.getResponseHeader(k);
        return this._resHeaders.get(k);
      }

      getAllResponseHeaders() {
        if (this._real) return this._real.getAllResponseHeaders();
        return [...this._resHeaders].map(([k, v]) => `${k}: ${v}`).join("\r\n");
      }

      overrideMimeType() {}

      abort() {
        this._aborted = true;
        if (this._real) this._real.abort();
      }

      send(body) {
        if (this._real) return this._real.send(body);
        rbxRequest(this._url, { method: this._method || "GET", headers: this._headers, body })
          .then((res) => {
            if (this._aborted) return;
            const bytes = res.body ? b64ToBytes(res.body) : new Uint8Array(0);
            this.status = res.status;
            this._resHeaders = new Headers(res.headers || {});
            this.readyState = 4;
            if (this.responseType === "arraybuffer") this.response = bytes.buffer;
            else if (this.responseType === "blob") this.response = new Blob([bytes]);
            else {
              this.responseText = new TextDecoder().decode(bytes);
              this.response = this.responseText;
            }
            this._fire("readystatechange");
            this._fire("load");
            if (typeof this.onload === "function") this.onload({ target: this });
          })
          .catch((e) => this._fail(e));
      }

      _syncFromReal() {
        const r = this._real;
        this.status = r.status;
        this.response = r.response;
        this.responseText = r.responseText;
        this.readyState = r.readyState;
        if (typeof this.onload === "function") this.onload({ target: this });
      }

      _fail(e) {
        this.readyState = 4;
        this.status = 0;
        if (typeof this.onerror === "function") this.onerror(e || { target: this });
      }

      _fire(type) {
        const ev = new Event(type);
        this.dispatchEvent(ev);
        const handler = this["on" + type];
        if (typeof handler === "function") handler.call(this, ev);
      }
    }
    for (const k of ["UNSENT", "OPENED", "HEADERS_RECEIVED", "LOADING", "DONE"]) {
      FrameXHR[k] = OrigXHR[k];
    }
    FrameXHR.prototype.UNSENT = 0;
    FrameXHR.prototype.OPENED = 1;
    FrameXHR.prototype.HEADERS_RECEIVED = 2;
    FrameXHR.prototype.LOADING = 3;
    FrameXHR.prototype.DONE = 4;
    self.XMLHttpRequest = FrameXHR;
  }

  self.__rbxNetHosts = [...HOSTS];
})();
