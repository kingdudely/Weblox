"use strict";

(() => {
  document.documentElement?.setAttribute?.("data-rpb-hook", "1");
  if (location.origin !== "https://play.roblox.com" ||
      location.pathname !== "/games/start") return;

  const placeId = new URLSearchParams(location.search).get("placeId");
  if (!/^[1-9]\d*$/.test(placeId || "") ||
      !Number.isSafeInteger(Number(placeId))) return;

  function installJoinPatch(scope) {
    const Xhr = scope.XMLHttpRequest;
    if (!Xhr?.prototype) return;
    if (scope.fetch && !scope.fetch.__oracle) {
      const of = scope.fetch;
      const wf = function (...a) {
        try {
          const u = typeof a[0] === "string" ? a[0] : (a[0] && a[0].url) || "?";
          const txt = "[oracle] " + (scope === window ? "window" : "worker:" + (scope.name || "?")) +
            " fetch " + u + "\n" + new Error().stack.split("\n").slice(1, 16).join("\n");
          try { (scope.__BC || (scope.__BC = new BroadcastChannel("rbxwire"))).postMessage(["oracle", 0, txt]); } catch (e) {}
          console.log(txt);
        } catch (e) {}
        return of.apply(scope, a);
      };
      wf.__oracle = 1;
      scope.fetch = wf;
    }
    if (!Xhr.prototype.__oracle) {
      Xhr.prototype.__oracle = 1;
      const snd = Xhr.prototype.send, opn = Xhr.prototype.open;
      Xhr.prototype.open = function (m, u, ...r) { this.__u = u; return opn.call(this, m, u, ...r); };
      const sh = Xhr.prototype.setRequestHeader;
      Xhr.prototype.setRequestHeader = function (k, v) {
        try {
          const t = "[oracle] hdr " + k + ": " + String(v).slice(0, 120);
          (scope === window ? window.__BC2 : scope.__BC || (scope.__BC = new BroadcastChannel("rbxwire")))
            .postMessage(["oracle", 0, t]);
        } catch (e) {}
        return sh.call(this, k, v);
      };
      Xhr.prototype.send = function (...a) {
        try {
          const where = scope === window ? "window" : "worker:" + (scope.name || "?");
          const txt = "[oracle] " + where + " send " + this.__u + "\n" +
            new Error().stack.split("\n").slice(1, 16).join("\n");
          try { (scope.__BC || (scope.__BC = new BroadcastChannel("rbxwire"))).postMessage(["oracle", 0, txt]); } catch (e) {}
          console.log(txt);
        } catch (e) {}
        return snd.apply(this, a);
      };
    }

    const JOIN_ORIGIN = "https://gamejoin.roblox.com";
    const JOIN_PATHS = new Set([
      "/v1/join-game",
      "/v1/join-game-instance",
      "/v1/join-reserved-game",
      "/v1/join-private-game"
    ]);
    const DOMAIN = "qdmux.roblox.com";
    const states = new WeakMap();
    const proto = Xhr.prototype;
    const originalOpen = proto.open;
    const originalSetHeader = proto.setRequestHeader;
    const originalSend = proto.send;
    const responseDescriptor = Object.getOwnPropertyDescriptor(proto, "response");
    if (!responseDescriptor?.configurable || !responseDescriptor.get) return;

    function ipv4Parts(value) {
      if (typeof value !== "string") throw new Error("Missing IPv4 address");
      const parts = value.split(".").map(Number);
      if (parts.length !== 4 || parts.some((part) =>
        !Number.isInteger(part) || part < 0 || part > 255)) {
        throw new Error("Invalid IPv4 address");
      }
      return parts;
    }

    function ipv4Hex(parts) {
      return parts.map((part) => part.toString(16).padStart(2, "0")).join("");
    }

    function tokenHex(value) {
      if (typeof value !== "string") throw new Error("Missing network token");
      if (/^[0-9a-f]{32}$/i.test(value)) return value.toLowerCase();
      let binary;
      try {
        binary = atob(value);
      } catch {
        throw new Error("Invalid network token encoding");
      }
      if (binary.length !== 16) throw new Error("Invalid network token length");
      return Array.from(binary, (character) =>
        character.charCodeAt(0).toString(16).padStart(2, "0")).join("");
    }

    function patchResponse(raw) {
      let response;
      try {
        response = JSON.parse(raw);
      } catch {
        return null;
      }
      if (response?.status !== 2) return null;

      const scriptIsString = typeof response.joinScript === "string";
      let joinScript;
      try {
        joinScript = scriptIsString ? JSON.parse(response.joinScript) : response.joinScript;
      } catch {
        return null;
      }
      if (!joinScript || typeof joinScript !== "object" || joinScript.gameFqdn) {
        return null;
      }

      try {
        const nativeVip = ipv4Parts(joinScript.UdmuxEndpoints?.[0]?.Address);
        if (nativeVip[3] % 2 !== 1 || nativeVip[3] === 255) return null;
        const browserVip = [...nativeVip];
        browserVip[3] += 1;
        const machine = ipv4Parts(joinScript.MachineAddress);
        const port = Number(joinScript.NetStackPort);
        if (!Number.isInteger(port) || port < 1 || port > 65535) return null;

        joinScript.gameFqdn = `${tokenHex(joinScript.NetStackTokenValue)}-` +
          `${ipv4Hex(machine)}-${port.toString(16).padStart(4, "0")}.` +
          `${ipv4Hex(browserVip)}.${DOMAIN}`;
        response.joinScript = scriptIsString ? JSON.stringify(joinScript) : joinScript;
        return JSON.stringify(response);
      } catch {
        return null;
      }
    }

    proto.open = function (method, url, ...rest) {
      let join = false;
      try {
        const parsed = new URL(String(url), scope.location.href);
        join = String(method).toUpperCase() === "POST" &&
          parsed.origin === JOIN_ORIGIN && JOIN_PATHS.has(parsed.pathname);
      } catch {
        // Let the browser handle an invalid URL as it normally would.
      }
      states.set(this, { join, headerSet: false, original: null, patched: null });
      return originalOpen.call(this, method, url, ...rest);
    };

    proto.setRequestHeader = function (name, value) {
      const state = states.get(this);
      if (state?.join && String(name).toLowerCase() === "rbx-user-agent") {
        if (state.headerSet) return;
        state.headerSet = true;
        value = "Roblox/Darwin";
      }
      return originalSetHeader.call(this, name, value);
    };

    proto.send = function (body) {
      const state = states.get(this);
      if (state?.join && !state.headerSet) {
        originalSetHeader.call(this, "rbx-user-agent", "Roblox/Darwin");
        state.headerSet = true;
      }
      return originalSend.call(this, body);
    };

    Object.defineProperty(proto, "response", {
      configurable: true,
      enumerable: responseDescriptor.enumerable,
      get() {
        const original = responseDescriptor.get.call(this);
        const state = states.get(this);
        if (!state?.join || this.readyState !== 4 || this.status !== 200 ||
            !(original instanceof ArrayBuffer)) return original;
        if (state.original === original) return state.patched;
        state.original = original;
        const body = patchResponse(new TextDecoder().decode(original));
        state.patched = body === null ? original : new TextEncoder().encode(body).buffer;
        return state.patched;
      }
    });
  }

  console.log("HOOK-A");
  const bc2 = () => (window.__BC2 || (window.__BC2 = new BroadcastChannel("rbxwire")));
  installJoinPatch(window);
  console.log("HOOK-B", typeof bc2);
  try { bc2().postMessage(["oracle", 0, "HOOK-C"]); } catch (e) { console.log("bc2 fail", e); }


  console.log("HOOK-D", !!WebAssembly.Instance, WebAssembly.Instance && WebAssembly.Instance.__o);
  if (WebAssembly.Instance && !WebAssembly.Instance.__o) {
    console.log("HOOK-E");
    const OldI = WebAssembly.Instance;
    const NewI = class extends OldI {
      constructor(m, i) {
        try {
          if (i && i.env) {
            const env = i.env;
            for (const k of ["_emscripten_notify_mailbox_postmessage", "__pthread_create_js", "rbx_wasm_rna_connect_js", "rbx_wasm_rna_is_supported_js"]) {
              const f = env[k];
              if (typeof f !== "function" || f.__o) continue;
              const w2 = function (...a) {
                try {
                  let extra = "";
                try {
                  if (k === "rbx_wasm_rna_connect_js" && typeof a[1] === "number") {
                    const u8 = new Uint8Array(imports.env.memory.buffer);
                    let e = a[1] >>> 0, out = "";
                    while (u8[e] && out.length < 300) out += String.fromCharCode(u8[e++]);
                    extra = " url=" + out;
                  }
                } catch (e) {}
                bc2().postMessage(["oracle", 0, "[oracle] I " + k + " " +
                    a.map((x) => typeof x === "number" ? "0x" + (x >>> 0).toString(16) : String(x)).join(",") + extra]);
                } catch (e) {}
                return f.apply(this, a);
              };
              w2.__o = 1;
      env[k] = w2;
            }
          }
        } catch (e) {}
        super(m, i);
      }
    };
    NewI.__o = 1;
    WebAssembly.Instance = NewI;
    bc2().postMessage(["oracle", 0, "[oracle] instance hook installed"]);
  }
  const wi = WebAssembly.instantiate;
  if (wi && !wi.__o) {
    const patched = function (src, imports) {
      try {
        if (imports && imports.env) {
          const env = imports.env;
          for (const k of ["_emscripten_notify_mailbox_postmessage", "__pthread_create_js", "rbx_wasm_rna_connect_js", "rbx_wasm_rna_is_supported_js"]) {
            const f = env[k];
            if (typeof f !== "function" || f.__o) continue;
            const w2 = function (...a) {
              try {
                bc2().postMessage(["oracle", 0, "[oracle] " + (typeof self.name === "string" && self.name ? "w:" + self.name : "main") +
                  " " + k + " " + a.map((x) => typeof x === "number" ? "0x" + (x >>> 0).toString(16) : String(x)).join(",") +
                  "\n" + new Error().stack.split("\n").slice(2, 8).join("\n")]);
              } catch (e) {}
              return f.apply(this, a);
            };
            w2.__o = 1;
            env[k] = w2;
          }
        }
      } catch (e) {}
      return wi.call(this, src, imports);
    };
    patched.__o = 1;
    WebAssembly.instantiate = patched;
  }


  // Emscripten only promisifies its own async exports, so a wasm event handler
  // that has to wait on the main thread suspends without a promising frame and
  // the click is dropped ("SuspendError").  Promisify the DOM entry instead.
  if (WebAssembly.promising) {
    const wraps = new WeakMap();
    const wrap = (f) => {
      if (typeof f !== "function") return f;
      let w = wraps.get(f);
      if (!w) {
        let p = f;
        try { p = WebAssembly.promising(f); } catch { p = f; }
        w = { p };
        wraps.set(f, w);
      }
      return w.p;
    };
    const ael = EventTarget.prototype.addEventListener;
    EventTarget.prototype.addEventListener = function (t, f, o) {
      return ael.call(this, t, wrap(f), o);
    };
    const rel = EventTarget.prototype.removeEventListener;
    EventTarget.prototype.removeEventListener = function (t, f, o) {
      return rel.call(this, t, (f && wraps.get(f)?.p) || f, o);
    };
  }

  try {
    new BroadcastChannel("rbxwire").onmessage = (e) => {
      if (Array.isArray(e.data) && e.data[0] === "oracle") console.log(e.data[2]);
    };
  } catch (e) {}

  // the page may create the transport itself; mirror the worker wrapper here
  (() => {
    const N = window.WebTransport;
    if (!N) return;
    const bc = () => (window.__BC2 || (window.__BC2 = new BroadcastChannel("rbxwire")));
    const hex = (u) => Array.from(new Uint8Array(u.buffer ? u.buffer : u)).map((c) => c.toString(16).padStart(2, "0")).join("");
    const track = (d, u) => { try { bc().postMessage([d, u.byteLength || 0, hex(u)]); } catch {} };
    window.WebTransport = class extends N {
      constructor(...a) {
        super(...a);
        const t = this;
        const wrapSt = (getWriter) => ({ getWriter() { const w = getWriter(); const wr = w.write.bind(w); w.write = (u) => { track("tx", u); return wr(u); }; return w; } });
        const odg = t.datagrams;
        if (odg) Object.defineProperty(t, "datagrams", { configurable: true, get: () => ({ maxDatagramSize: odg.maxDatagramSize, get writable() { return wrapSt(odg.writable.getWriter.bind(odg.writable)); } }) });
        const ocu = N.prototype.createUnidirectionalStream;
        t.createUnidirectionalStream = () => { const st = ocu.call(t); return wrapSt(st.getWriter.bind(st)); };
        const oi = t.incomingUnidirectionalStreams.getReader.bind(t.incomingUnidirectionalStreams);
        t.incomingUnidirectionalStreams = { getReader() { const r = oi(); return { read: async () => { const x = await r.read(); if (x.value) { const rr = x.value.getReader(); const or = rr.read.bind(rr); rr.read = async () => { const c = await or(); if (c.value) track("rx", c.value); return c; }; x.value = { getReader: () => rr }; } return x; } }; } };
        try { bc().postMessage(["url", 0, String(a[0])]); } catch {}
        Promise.resolve().then(() => { try { bc().postMessage(["pwt", 0, String(window.WebTransport).slice(0, 30)]); } catch {} });
      }
    };
  })();


  const NativeWorker = window.Worker;
  if (typeof NativeWorker === "function") {
    const workerUrls = new Set();
    window.Worker = class extends NativeWorker {
      constructor(url, options) {
        const g = self; g.__W = g.__W || { n: 0, m: 0, names: [] }; g.__W.n++; g.__W.names.push(options?.name);
        console.log("[oracle] new Worker " + String(url).slice(0, 90) + " name=" + options?.name);
        let engineUrl = null;
        try {
          const stub = new URL(String(url), location.href);
          const source = new URL(stub.searchParams.get("src") || "");
          if (stub.origin === location.origin &&
              stub.pathname.endsWith("/assets/webclient/stub.js") &&
              options?.name === "em-pthread" &&
              source.origin === "https://setup.rbxcdn.com" &&
              /^\/webclient\/[^/]+\/WebClient\.js$/.test(source.pathname)) {
            engineUrl = source.href; self.__W.m++;
          }
        } catch {
          // Other workers keep their normal URL and constructor behavior.
        }
        console.log("[oracle] wire? " + String(!!engineUrl) + " " + String(url).slice(0, 120));
        if (!engineUrl) {
          super(url, options);
          return;
        }

        const wire = `
(() => {
  const bc = () => (self.__BC || (self.__BC = new BroadcastChannel("rbxwire")));
  const say0 = (t) => { try { bc().postMessage(["oracle", 0, t]); } catch (e) {} };
  say0("wire start");
  try {
  const hex = (u) => Array.from(new Uint8Array(u.buffer ? u.buffer : u)).map((c) => c.toString(16).padStart(2, "0")).join("");
  const say = (t) => { try { bc().postMessage(["oracle", 0, t]); } catch {} };
  const track = (d, u) => { try { bc().postMessage([d, u.byteLength || 0, hex(u)]); } catch {} };
  const w = self.WritableStreamDefaultWriter && self.WritableStreamDefaultWriter.prototype;
  if (w && w.write) { const ow = w.write; w.write = function (u) { track("tx", u); return ow.call(this, u); }; }
  say0("m1");
  const r = self.ReadableStreamDefaultReader && self.ReadableStreamDefaultReader.prototype;
  if (r && r.read) { const or = r.read; r.read = async function () { const x = await or.call(this); if (x && x.value) track("rx", x.value); return x; }; }
  const N = self.WebTransport;
  if (N) self.WebTransport = class extends N { constructor(...a) { super(...a); say("wt " + String(a[0])); } };

  say0("m2");
  const CSTR = (env, p) => {
    try {
      const u8 = new Uint8Array(env.memory.buffer);
      let e = p >>> 0, out = "";
      while (u8[e] && out.length < 400) out += String.fromCharCode(u8[e++]);
      return JSON.stringify(out);
    } catch (e) { return "?"; }
  };
  const BYTES = (env, p, n) => {
    try {
      const u8 = new Uint8Array(env.memory.buffer);
      return hex(u8.subarray(p >>> 0, (p >>> 0) + Math.min(n >>> 0, 64)));
    } catch (e) { return "?"; }
  };
  function hook(env) {
    let n = 0;
    for (const k of Object.keys(env)) {
      const f = env[k];
      if (typeof f !== "function") continue;
      if (k.indexOf("rbx_wasm_rna_") !== 0 && k !== "_emscripten_notify_mailbox_postmessage" && k !== "__pthread_create_js") continue;
      if (f.__o) continue;
      const w2 = function (...a) {
        try {
          let extra = "";
          if (/connect_js$/.test(k)) extra = " url=" + CSTR(env, a[1]) + " slots=" + a[7] + "x" + a[8] + " data=0x" + (a[4] >>> 0).toString(16);
          else if (/write_stream_js$/.test(k)) extra = " sid=" + a[1] + " len=" + a[3] + " " + BYTES(env, a[2], a[3]);
          else if (/send_datagram_js$/.test(k)) extra = " len=" + a[2] + " " + BYTES(env, a[1], a[2]);
          else if (/open_stream_js$/.test(k)) extra = " sid=" + a[1];
          else extra = " " + a.map((x) => typeof x === "number" ? "0x" + (x >>> 0).toString(16) : String(x)).join(",");
          say("rna " + k.replace("rbx_wasm_rna_", "") + extra);
        } catch (e) {}
        return f.apply(this, a);
      };
      w2.__o = 1;
      env[k] = w2;
      n++;
    }
    say("hooked " + n);
  }
  say0("m3");
  for (const name of ["instantiate", "instantiateStreaming"]) {
    const wi = WebAssembly[name];
    if (!wi) continue;
    WebAssembly[name] = function (src, imports) {
      try { if (imports && imports.env) hook(imports.env); } catch (e) {}
      try { return wi.call(this, src, imports); }
      catch (e) { say("instantiate threw " + e); throw e; }
    };
  }
  if (WebAssembly.Instance) {
    const Old = WebAssembly.Instance;
    WebAssembly.Instance = class extends Old {
      constructor(m, i) { try { if (i && i.env) hook(i.env); } catch (e) {} super(m, i); }
    };
  }
  say("wired");
  } catch (e) { say("wirefail " + e); }
})();
`;
        const bootstrap = `(${installJoinPatch.toString()})(self);\n` + wire +
          `importScripts(${JSON.stringify(engineUrl)});`;
        const blobUrl = URL.createObjectURL(new Blob([bootstrap], {
          type: "text/javascript"
        }));
        console.log("[oracle] blob=" + bootstrap.slice(0, 300).replace(/\n/g, " | "));
        try {
          super(blobUrl, options);
          workerUrls.add(blobUrl);
        } catch (error) {
          URL.revokeObjectURL(blobUrl);
          throw error;
        }
      }
    };
    window.addEventListener("pagehide", () => {
      for (const url of workerUrls) URL.revokeObjectURL(url);
      workerUrls.clear();
    }, { once: true });
  }

  const fallbackUrl = `https://www.roblox.com/games/${placeId}`;
  let returnUrl = fallbackUrl;
  try {
    const supplied = new URL(new URLSearchParams(location.search).get("returnUrl"));
    if (supplied.origin === "https://www.roblox.com" &&
        supplied.pathname.startsWith(`/games/${placeId}/`) &&
        !supplied.username && !supplied.password) {
      returnUrl = supplied.origin + supplied.pathname;
    }
  } catch {
    // Direct Web Play links return to the matching game details page.
  }

  let resolveGameExit;
  Object.defineProperty(window, "__resolveGameExit", {
    configurable: true,
    get() { return resolveGameExit; },
    set(callback) {
      resolveGameExit = typeof callback !== "function" ? callback : (result) => {
        if (result?.reason === "leave" || result?.reason === "native_exit") {
          location.replace(returnUrl);
          return;
        }
        return callback(result);
      };
    }
  });

  document.addEventListener("click", (event) => {
    if (event.target instanceof Element &&
        event.target.closest("#loading-cancel, #loading-error-leave")) {
      event.preventDefault();
      event.stopImmediatePropagation();
      location.replace(returnUrl);
    }
  }, true);
})();

// ---- temporary oracle: who calls XHR on the page? -------------------------
(() => {
  const X = window.XMLHttpRequest;
  if (!X?.prototype) return;
  const send = X.prototype.send;
  X.prototype.send = function (...a) {
    try {
      const st = new Error().stack.split("\n").slice(1, 14).join("\n");
      console.log("[oracle] XHR send " + this.__url + "\n" + st);
    } catch (e) {}
    return send.apply(this, a);
  };
  const open = X.prototype.open;
  X.prototype.open = function (m, u, ...r) {
    this.__url = u;
    return open.call(this, m, u, ...r);
  };
})();

setTimeout(() => document.getElementById("canvas")?.dispatchEvent(new MouseEvent("click", { bubbles: true })), 9000);
setTimeout(() => document.dispatchEvent(new MouseEvent("click", { bubbles: true })), 12000);
