// MAIN-world hook, document_start.
//
// Two jobs:
//   1. force the engine onto RakNet (the WebTransport path cannot join places
//      the account is not enrolled in),
//   2. pump the datagram rings that tools/patch.py spliced into the module.
//
// The patched module has no sockets at all, so it hands every outgoing
// datagram to a ring in linear memory and reads replies from a second ring
// (see tools/patch.py: RING_BASE). Emscripten builds pthread modules with
// `shared: true`, which means the page and every engine worker already share
// one SharedArrayBuffer — so the page can read those rings directly. This
// script captures that buffer by wrapping WebAssembly.Memory, then forwards
// both directions over a WebSocket to tools/udp_bridge.js, which owns the real
// UDP socket.
(() => {
  if (globalThis.__rbxRaknetHook) return;
  globalThis.__rbxRaknetHook = 1;

  // ---------------------------------------------------------------- flags ---
  const of = self.fetch;
  self.fetch = function (input, init) {
    const url = typeof input === "string" ? input : (input && input.url) || "";
    const p = of.call(self, input, init);
    if (/\/v2\/settings\/application\/WebClientApp/.test(url)) {
      return p.then(async (r) => {
        try {
          const j = await r.clone().json();
          j.applicationSettings = j.applicationSettings || {};
          // The web build has no socket layer, so the engine substitutes a
          // dummy transport that fakes a connection. Disable it so the join
          // actually goes through RakNet (whose socket path we patched).
          const flags = {
            FFlagUseRbxTransportClient3: "False",
            FFlagUseRbxTransportServer: "False",
            // dummy left at engine default for now: it is the build's stand-in
            // for the transport, and disabling it just stops the join silently.
          };
          Object.assign(j.applicationSettings, flags);
          console.log("[raknet] transport flag forced");
          return new Response(JSON.stringify(j), {
            status: 200,
            headers: { "content-type": "application/json" },
          });
        } catch (e) {
          return r;
        }
      });
    }
    return p;
  };

  // ------------------------------------------------------------- the ring ---
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
  const CREQ = RING + 0x38;
  const CRES = RING + 0x3c;
  const HOST = RING + 0x40;
  const PORT = RING + 0x70;
  const ADDR = RING + 0x20;
  const SLOT = 1536;
  const NSLOT = 64;
  const TX0 = RING + 0x80;
  const RX0 = TX0 + NSLOT * SLOT;
  const MAX_DATAGRAM = 1500;
  const MAGIC_VALUE = 0x4b52414e; // 'RAKN'

  const BRIDGE = "ws://127.0.0.1:37999/";

  let mem = null;      // WebAssembly.Memory
  let i32 = null;      // Int32Array over the whole buffer (atomics + headers)
  let u8 = null;       // Uint8Array over the whole buffer (payloads)
  let u32 = null;      // Uint32Array (slot lengths)
  let ws = null;
  let connected = false;

  const stats = { tx: 0, rx: 0, dropTx: 0, dropRx: 0, txBytes: 0, rxBytes: 0 };

  // Capture the engine's shared memory. Emscripten creates it as
  // `new WebAssembly.Memory({ initial, maximum, shared: true })`, so wrapping
  // the constructor is enough and needs nothing inside the worker.
  const RealMemory = WebAssembly.Memory;
  function Memory(desc) {
    const m = new RealMemory(desc);
    if (desc && desc.shared) attach(m);
    return m;
  }
  Memory.prototype = RealMemory.prototype;
  Object.setPrototypeOf(Memory, RealMemory);
  WebAssembly.Memory = Memory;

  function attach(m) {
    if (mem) return;
    mem = m;
    const sab = m.buffer;
    i32 = new Int32Array(sab);
    u8 = new Uint8Array(sab);
    u32 = new Uint32Array(sab);
    Atomics.store(i32, MAGIC >> 2, MAGIC_VALUE);
    console.log("[rkring] shared memory captured, " + sab.byteLength + " B");
    diagnose();
    connect();
    setInterval(pumpOut, 2);
  }

  function diagnose() {
    try {
      const meta = document.querySelector('meta[http-equiv="Content-Security-Policy"]');
      console.log("[rkdiag] csp meta=" + (meta ? meta.content.slice(0, 120) : "none"));
    } catch (e) {}
    const t0 = performance.now();
    fetch("http://127.0.0.1:37999/").then(
      (r) => console.log("[rkdiag] http ok status=" + r.status),
      (e) => console.log("[rkdiag] http err " + String(e) + " after " + (performance.now() - t0).toFixed(0) + "ms")
    );
  }

  function connect() {
    try {
      ws = new WebSocket(BRIDGE);
    } catch (e) {
      console.warn("[rkring] bridge socket failed", e);
      return;
    }
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      connected = true;
      console.log("[rkring] bridge connected " + BRIDGE);
    };
    ws.onclose = () => {
      connected = false;
      console.warn("[rkring] bridge closed, retrying");
      setTimeout(connect, 1000);
    };
    ws.onerror = (e) => console.warn("[rkring] bridge error state=" + ws.readyState);
    setTimeout(() => {
      if (ws.readyState !== 1) console.warn("[rkring] bridge state=" + ws.readyState);
    }, 3000);
    ws.onmessage = (ev) => enqueue(ev.data);
  }

  // The engine's own sockaddr, captured by the send patch, is the only
  // trustworthy description of where it thinks the server is.
  function peerAddr() {
    if (!u8) return null;
    const family = u8[ADDR] | (u8[ADDR + 1] << 8);
    if (family !== 2) return null;
    const port = (u8[ADDR + 2] << 8) | u8[ADDR + 3];
    const ip = u8[ADDR + 4] + "." + u8[ADDR + 5] + "." + u8[ADDR + 6] + "." + u8[ADDR + 7];
    if (!port || ip === "0.0.0.0") return null;
    return { ip, port, text: ip + ":" + port };
  }

  let autoDest = false;

  // wasm -> page -> bridge
  function pumpOut() {
    if (!connected) return;
    let tail = Atomics.load(i32, TX_TAIL >> 2);
    const head = Atomics.load(i32, TX_HEAD >> 2);
    if (!autoDest && tail !== head) {
      const a = peerAddr();
      // 127.0.0.1 means getaddrinfo is still synthetic; keep the launcher's
      // destination in that case (the port is still the engine's real one).
      if (a && a.ip !== "127.0.0.1") {
        autoDest = true;
        ws.send(JSON.stringify({ t: "dest", v: a.text }));
        console.log("[rkring] peer from captured sockaddr: " + a.text);
      }
    }
    while (tail !== head) {
      const base = TX0 + (tail & (NSLOT - 1)) * SLOT;
      const len = u32[base >> 2];
      if (len === 0 || len > MAX_DATAGRAM) break; // writer mid-flight
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

  // bridge -> page -> wasm
  function enqueue(buf) {
    const data = buf instanceof ArrayBuffer ? new Uint8Array(buf) : buf;
    if (!data || !data.length) return;
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
    // wake the engine's recv thread out of its futex park immediately
    Atomics.notify(i32, WAKE >> 2, 1);
    stats.rx++;
    stats.rxBytes += len;
  }

  // probing surface for tools/probe/tapprobe.js
  globalThis.__rkRing = {
    get ready() { return !!mem; },
    get connected() { return connected; },
    get stats() { return { ...stats }; },
    // The engine's real RCC address is only known to the engine (its
    // getaddrinfo is synthetic here), so the launcher relays it in from the
    // engine's own "UDMUX Address = ..., Port = ..." log line.
    // Ask the engine's own RakNet peer to open a connection. The peer pointer
    // is captured by UpdateNetworkLoop; the connect itself is issued from the
    // recv thread because that is the only thread we own that runs often.
    connect(ip, port) {
      if (!mem) return false;
      const bytes = new TextEncoder().encode(ip + "\u0000");
      if (bytes.length > 48) return false;
      u8.set(bytes, HOST);
      Atomics.store(i32, PORT >> 2, port | 0);
      Atomics.store(i32, CRES >> 2, 0);
      Atomics.store(i32, CREQ >> 2, 1);
      return true;
    },
    get connectResult() {
      return mem ? Atomics.load(i32, CRES >> 2) : null;
    },
    setDest(v) {
      if (!ws || ws.readyState !== 1) return false;
      ws.send(JSON.stringify({ t: "dest", v }));
      return true;
    },
    get wasm() {
      return mem && {
        selects: Atomics.load(i32, SELECTS >> 2),
        recvd: Atomics.load(i32, RECVD >> 2),
        magic: Atomics.load(i32, MAGIC >> 2),
        txHead: Atomics.load(i32, TX_HEAD >> 2),
        rxHead: Atomics.load(i32, RX_HEAD >> 2),
        rxTail: Atomics.load(i32, RX_TAIL >> 2),
        peer: peerAddr() && peerAddr().text,
        rakpeer: mem && (Atomics.load(i32, (RING + 0x30) >> 2) >>> 0).toString(16),
        updates: mem && Atomics.load(i32, (RING + 0x34) >> 2),
      };
    },
  };

  console.log("[raknet] hook installed");
})();
