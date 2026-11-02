// ring.js — datagram transport for the self-hosted page.
//
// The patched module has no sockets. tools/patch.py splices two rings into its
// linear memory (RING_BASE below); this file pumps them over a WebSocket to
// tools/udp_bridge.js, which owns the real UDP socket.
//
// It only works because the engine is built as a pthread module, so Emscripten
// creates its memory with `shared: true`: the page and every engine worker
// already share one SharedArrayBuffer. Wrapping WebAssembly.Memory at load time
// is enough to get hold of it — nothing needs to reach inside the worker.
//
// Served from our own origin, so the page is itself a local page; a WebSocket
// to another loopback port is not treated as a public->private request.
(() => {
  if (globalThis.__rkRing) return;


  // ---------------------------------------------------------------- audio ---
  // WebKitGTK reports AudioDestinationNode.maxChannelCount === 0 on this box,
  // even with PipeWire running and a real card present. Per spec that means the
  // channel count may not be changed, so `destination.channelCount =
  // destination.maxChannelCount` throws — and Emscripten's WebAudio backend
  // does exactly that during init, outside any try. The exception escapes
  // main_loop_iteration and the join never runs.
  //
  // Measured: every other node accepts channelCount = 2; only the destination
  // write fails. So report the device as stereo and absorb that one write.
  // Verified in MiniBrowser: worklet module loads, AudioWorkletNode with
  // outputChannelCount [2] builds, connect succeeds.
  function relaxAudio() {
    try {
      if (typeof AudioDestinationNode === "undefined") return;
      const dst = AudioDestinationNode.prototype;
      const node = typeof AudioNode !== "undefined" ? AudioNode.prototype : null;
      const count = node && Object.getOwnPropertyDescriptor(node, "channelCount");
      Object.defineProperty(dst, "maxChannelCount", { configurable: true, get: () => 2 });
      Object.defineProperty(dst, "channelCount", {
        configurable: true,
        get() {
          return count && count.get ? count.get.call(this) : 2;
        },
        set() {
          // WebKit has no device to size this against; leave the default 2.
        },
      });
      console.log("[rkring] destination channel clamp installed");
    } catch (e) {
      console.log("[rkring] audio clamp failed: " + e);
    }
  }
  relaxAudio();

  // ---------------------------------------------------------------- layout ---
  // Must match tools/patch.py exactly.
  const RING = 0x7ff00000;
  const MAGIC = RING + 0x00;
  const TX_HEAD = RING + 0x04;
  const TX_TAIL = RING + 0x08;
  const RX_HEAD = RING + 0x0c;
  const RX_TAIL = RING + 0x10;
  const WAKE = RING + 0x14;
  const SELECTS = RING + 0x18;
  const RECVD = RING + 0x1c;
  const ADDR = RING + 0x20;
  const PEER = RING + 0x30;
  const UPDATES = RING + 0x34;
  const CREQ = RING + 0x38;
  const CRES = RING + 0x3c;
  const HOST = RING + 0x40;
  const PORT = RING + 0x70;
  const SLOT = 1536;
  const NSLOT = 64;
  const TX0 = RING + 0x80;
  const RX0 = TX0 + NSLOT * SLOT;
  const MAX_DATAGRAM = 1500;
  const MAGIC_VALUE = 0x4b52414e; // 'RAKN'

  const BRIDGE_PORT = 37999;

  let mem = null, i32 = null, u8 = null, u32 = null, ws = null, connected = false;
  const stats = { tx: 0, rx: 0, dropRx: 0, txBytes: 0, rxBytes: 0, stamped: 0, routed: 0, session: 0 };

  const RealMemory = WebAssembly.Memory;
  function Memory(desc) {
    const m = new RealMemory(desc);
    if (desc && desc.shared) attach(m);
    return m;
  }
  Memory.prototype = RealMemory.prototype;
  Object.setPrototypeOf(Memory, RealMemory);
  WebAssembly.Memory = Memory;

  // ------------------------------------------------------------------ auth ---
  async function ensureCookie() {
    let status = { cookie: true };
    try {
      status = await (await fetch("/__status")).json();
    } catch (e) {}
    if (status.cookie) return;
    for (;;) {
      const v = window.prompt(
        "Paste your .ROBLOSECURITY cookie\n\n" +
          "It is sent only to the local proxy in this tab and never leaves this machine."
      );
      if (!v) return;
      await fetch("/__cookie", { method: "POST", body: v.trim() });
      const s = await (await fetch("/__status")).json();
      if (s.cookie) return;
      alert("That cookie was rejected by the proxy.");
    }
  }

  // --------------------------------------------------------------- the ring ---
  function attach(m) {
    if (mem) return;
    mem = m;
    const sab = m.buffer;
    i32 = new Int32Array(sab);
    u8 = new Uint8Array(sab);
    u32 = new Uint32Array(sab);
    Atomics.store(i32, MAGIC >> 2, MAGIC_VALUE);
    console.log("[rkring] shared memory captured, " + sab.byteLength + " B");
    connect();
    setInterval(pumpOut, 2);
    pollPeer();
  }

  function connect() {
    ws = new WebSocket("ws://127.0.0.1:" + BRIDGE_PORT + "/");
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      connected = true;
      console.log("[rkring] bridge connected");
      pollPeer();
    };
    ws.onclose = () => {
      connected = false;
      setTimeout(connect, 1000);
    };
    ws.onerror = () => {};
    ws.onmessage = (ev) => enqueue(ev.data);
  }

  // The engine's own sockaddr, captured by the send patch: the only trustworthy
  // description of where it believes the server is.
  function peerAddr() {
    if (!u8) return null;
    const family = u8[ADDR] | (u8[ADDR + 1] << 8);
    if (family !== 2) return null;
    const port = (u8[ADDR + 2] << 8) | u8[ADDR + 3];
    const ip = [4, 5, 6, 7].map((i) => u8[ADDR + i]).join(".");
    if (!port || ip === "0.0.0.0") return null;
    return { ip, port, text: ip + ":" + port };
  }

  let autoDest = false;
  let joined = false;
  let ruppToken = null;
  let rccIp = null;
  let rccPort = 0;
  let sessionToken = null; // TLV1 subtype 2, issued by the server in Reply2
  let dataPort = 0;        // TLV2 port for subtype-2 datagrams

  // The engine stamps a randomly generated RUPP token on the handshake, and the
  // server silently drops anything that does not carry the join script's
  // NetStackTokenValue. Rewrite TLV1 (subtype 1) in place on the way out.
  function stampRupp(off, len) {
    if ((!ruppToken && !rccIp) || len < 8) return;
    if (u8[off] !== 0x01 || u8[off + 1] !== 0x00) return;
    const total = (u8[off + 2] << 8) | u8[off + 3];
    if (total < 4 || total > len) return;
    const end = off + total;
    let p = off + 4;
    let sub = 0;
    while (p + 2 <= end) {
      const type = u8[p];
      const tlen = u8[p + 1];
      if (p + 2 + tlen > end) break;
      if (type === 1 && tlen === 17) {
        sub = u8[p + 2];
        if (sub === 1 && ruppToken) {
          u8.set(ruppToken, p + 3);
          stats.stamped++;
        } else if (sub === 2 && sessionToken) {
          u8.set(sessionToken, p + 3);
          stats.session++;
        }
      } else if (type === 2 && tlen === 6 && rccIp) {
        // Handshake datagrams route on the NetStack port; established-session
        // datagrams must advertise the RCC data port instead.
        const port = sub === 2 && dataPort ? dataPort : rccPort;
        u8.set(rccIp, p + 2);
        u8[p + 6] = (port >> 8) & 0xff;
        u8[p + 7] = port & 0xff;
        stats.routed++;
      }
      p += 2 + tlen;
    }
  }

  function pumpOut() {
    if (!connected) return;
    let tail = Atomics.load(i32, TX_TAIL >> 2);
    const head = Atomics.load(i32, TX_HEAD >> 2);
    if (!autoDest && tail !== head) {
      const a = peerAddr();
      // 127.0.0.1 means getaddrinfo is still synthetic; keep the server-side
      // peer in that case (the port it reports is the engine's real one).
      if (a && a.ip !== "127.0.0.1") {
        autoDest = true;
        ws.send(JSON.stringify({ t: "dest", v: a.text }));
        console.log("[rkring] peer from captured sockaddr: " + a.text);
      }
    }
    while (tail !== head) {
      const base = TX0 + (tail & (NSLOT - 1)) * SLOT;
      const len = u32[base >> 2];
      if (len === 0 || len > MAX_DATAGRAM) break;
      stampRupp(base + 4, len);
      try {
        ws.send(u8.slice(base + 4, base + 4 + len));
      } catch (e) {
        break;
      }
      stats.tx++;
      stats.txBytes += len;
      tail++;
      Atomics.store(i32, TX_TAIL >> 2, tail);
    }
  }

  function enqueue(buf) {
    const data = buf instanceof ArrayBuffer ? new Uint8Array(buf) : buf;
    if (!data || !data.length) return;
    learnSessionToken(data);
    const len = Math.min(data.length, MAX_DATAGRAM);
    const head = Atomics.load(i32, RX_HEAD >> 2);
    const tail = Atomics.load(i32, RX_TAIL >> 2);
    if (head - tail >= NSLOT) {
      stats.dropRx++;
      return;
    }
    const base = RX0 + (head & (NSLOT - 1)) * SLOT;
    u8.set(data.subarray(0, len), base + 4);
    u32[base >> 2] = len;
    Atomics.store(i32, RX_HEAD >> 2, head + 1);
    Atomics.notify(i32, WAKE >> 2, 1); // wake the engine's recv thread
    stats.rx++;
    stats.rxBytes += len;
  }

  // Reply2 carries the session token the edge expects on every later datagram.
  // Without echoing it the server ACKs nothing and the session dies.
  function learnSessionToken(d) {
    if (d.length < 8 || d[0] !== 0x01 || d[1] !== 0x00) return;
    const total = (d[2] << 8) | d[3];
    if (total < 4 || total > d.length) return;
    let p = 4;
    while (p + 2 <= total) {
      const type = d[p];
      const tlen = d[p + 1];
      if (p + 2 + tlen > total) break;
      if (type === 1 && tlen === 17 && d[p + 2] === 2) {
        const tok = d.slice(p + 3, p + 19);
        if (!sessionToken || String(sessionToken) !== String(tok)) {
          sessionToken = tok;
          console.log("[rkring] session token from server: " + Array.from(tok, (b) => b.toString(16).padStart(2, "0")).join(""));
        }
        return;
      }
      p += 2 + tlen;
    }
  }

  // ------------------------------------------------------------- the peer ---
  // The proxy sniffs the join response, so the page never needs to see inside
  // the engine's worker to learn the server address.
  function askConnect(p) {
    if (!mem || !connected) return false;
    const text = p.ip + "\u0000";
    if (text.length > 48) return false;
    u8.set(new TextEncoder().encode(text), HOST);
    Atomics.store(i32, PORT >> 2, p.port | 0);
    Atomics.store(i32, CRES >> 2, 0);
    Atomics.store(i32, CREQ >> 2, 1);
    console.log("[rkring] RakPeer::Connect -> " + p.ip + ":" + p.port);
    return true;
  }

  let peerTries = 0;
  async function pollPeer() {
    if (!mem || !connected) return;
    peerTries++;
    try {
      const p = await (await fetch("/__peer")).json();
      if (p && p.token && p.token.length === 32) {
        const h = p.token;
        ruppToken = new Uint8Array(16);
        for (let i = 0; i < 16; i++) ruppToken[i] = parseInt(h.substr(i * 2, 2), 16);
        console.log("[rkring] RUPP join token armed");
      }
      if (p && p.rcc && p.nsport) {
        const parts = p.rcc.split(".").map(Number);
        if (parts.length === 4 && parts.every((n) => n >= 0 && n <= 255)) {
          rccIp = new Uint8Array(parts);
          rccPort = p.nsport & 0xffff;
          dataPort = (p.sport || 0) & 0xffff;
          console.log("[rkring] RUPP TLV2 -> " + p.rcc + ":" + rccPort + " (data port " + dataPort + ")");
        }
      }
      if (p && p.ip && p.port) {
        ws.send(JSON.stringify({ t: "dest", v: p.ip + ":" + p.port }));
        joined = true;
        if ((!ruppToken || !rccIp) && peerTries < 20) {
          setTimeout(pollPeer, 500);
        }
        return;
      }
    } catch (e) {}
    setTimeout(pollPeer, 500);
  }

  globalThis.__rkRing = {
    get ready() { return !!mem; },
    get connected() { return connected; },
    get stats() { return { ...stats }; },
    get wasm() {
      if (!mem) return null;
      const a = peerAddr();
      return {
        selects: Atomics.load(i32, SELECTS >> 2),
        recvd: Atomics.load(i32, RECVD >> 2),
        peer: a && a.text,
        rakpeer: (Atomics.load(i32, PEER >> 2) >>> 0).toString(16),
        updates: Atomics.load(i32, UPDATES >> 2),
        connectResult: Atomics.load(i32, CRES >> 2),
        txHead: Atomics.load(i32, TX_HEAD >> 2),
        rxHead: Atomics.load(i32, RX_HEAD >> 2),
        rxTail: Atomics.load(i32, RX_TAIL >> 2),
      };
    },
    connect: askConnect,
    setDest(v) {
      if (!ws || ws.readyState !== 1) return false;
      ws.send(JSON.stringify({ t: "dest", v }));
      return true;
    },
  };


  // ------------------------------------------------------------- telemetry ---
  // The Tauri webview has no console a terminal can read, so the page reports
  // itself back to the local host instead. Everything here is throwaway once
  // the join is stable.
  const recent = [];
  let pending = [];

  function describe(x) {
    if (typeof x === "string") return x;
    if (x instanceof Error) return x.name + ": " + x.message + (x.stack ? "\n" + x.stack.split("\n").slice(0, 4).join("\n") : "");
    if (x && typeof x === "object") {
      try {
        const j = JSON.stringify(x);
        if (j && j !== "{}") return j;
      } catch (e) {}
      const bits = [];
      for (const k of ["name", "message", "stack", "type", "code", "reason"]) {
        if (x[k]) bits.push(k + "=" + String(x[k]).slice(0, 300));
      }
      return bits.length ? "{" + bits.join(", ") + "}" : String(x);
    }
    return String(x);
  }

  for (const k of ["log", "warn", "error", "info", "debug"]) {
    const orig = console[k].bind(console);
    console[k] = (...a) => {
      const line = a.map(describe).join(" ").slice(0, 800);
      recent.push(k + "|" + line);
      if (recent.length > 60) recent.shift();
      pending.push(k + "|" + line);
      if (pending.length > 400) pending.shift();
      orig(...a);
    };
  }

  function flushLogs() {
    if (pending.length) {
      const batch = pending;
      pending = [];
      try { fetch("/__log", { method: "POST", body: JSON.stringify(batch) }); } catch (e) {}
    }
    setTimeout(flushLogs, 400);
  }

  let posted = 0;
  function telemetry() {
    const me = globalThis.__rkRing;
    const body = JSON.stringify({
      age: Math.round(performance.now()),
      coi: typeof crossOriginIsolated === "undefined" ? null : crossOriginIsolated,
      sab: typeof SharedArrayBuffer !== "undefined",
      jspi: typeof WebAssembly.Suspending === "function",
      wasm: me ? me.wasm : null,
      stats: me ? me.stats : null,
      log: recent.slice(-20),
    });
    try { fetch("/__client", { method: "POST", body }); } catch (e) {}
    setTimeout(telemetry, 1500);
  }

  ensureCookie();
  telemetry();
  flushLogs();
  console.log("[raknet] ring hook installed");
})();
