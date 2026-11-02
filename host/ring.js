// ring.js — the datagram seam.
//
// The engine's RakNet is complete: it selects the transport, parses the join
// script, calls Connect, builds RUPP, retransmits and replicates. What it does
// not have is a socket, because a browser has none. tools/patch.py therefore
// splices three syscalls (sendto, recvfrom, poll) into two rings in linear
// memory, and this file moves bytes between those rings and rkhost over a
// WebSocket. Nothing here knows about RakNet.
//
// It works because the engine is a pthread build: Emscripten creates its memory
// with `shared: true`, so page and workers already share one
// SharedArrayBuffer. Wrapping WebAssembly.Memory at load time is enough.
(() => {
  if (globalThis.__rkRing) return;

  // ---------------------------------------------------------------- audio ---
  // WebKitGTK reports AudioDestinationNode.maxChannelCount === 0 here. Per spec
  // that means the count may not be changed, and Emscripten's WebAudio backend
  // assigns exactly that value during init, outside any try — the
  // NotSupportedError escapes main_loop_iteration and the join never starts.
  try {
    if (typeof AudioDestinationNode !== "undefined") {
      const dst = AudioDestinationNode.prototype;
      const node = typeof AudioNode !== "undefined" ? AudioNode.prototype : null;
      const count = node && Object.getOwnPropertyDescriptor(node, "channelCount");
      Object.defineProperty(dst, "maxChannelCount", { configurable: true, get: () => 2 });
      Object.defineProperty(dst, "channelCount", {
        configurable: true,
        get() {
          return count && count.get ? count.get.call(this) : 2;
        },
        set() {},
      });
    }
  } catch (e) {}

  // ---------------------------------------------------------------- layout ---
  // Written by tools/patch.py, injected by rkhost as window.__rkAbi, so the
  // offsets exist in exactly one place. The fallback is only for opening the
  // file without the server.
  const ABI = globalThis.__rkAbi || {
    base: 0x7ff00000, magic: 0x00, txHead: 0x04, txTail: 0x08, rxHead: 0x0c,
    rxTail: 0x10, wake: 0x14, selects: 0x18, recvd: 0x1c, addr: 0x20,
    peer: 0x30, updates: 0x34, slot: 1536, slots: 64, tx0: 0x80,
    rx0: 0x80 + 64 * 1536, maxDatagram: 1500,
  };
  const at = (off) => ABI.base + off;
  const MAGIC = at(ABI.magic);
  const TX_HEAD = at(ABI.txHead);
  const TX_TAIL = at(ABI.txTail);
  const RX_HEAD = at(ABI.rxHead);
  const RX_TAIL = at(ABI.rxTail);
  const WAKE = at(ABI.wake);       // futex word: page notifies, recv waits
  const SLOT = ABI.slot;
  const NSLOT = ABI.slots;
  const TX0 = at(ABI.tx0);
  const RX0 = at(ABI.rx0);
  const MAX_DATAGRAM = ABI.maxDatagram;
  const MAGIC_VALUE = 0x4b52414e; // 'RAKN'

  const BRIDGE_PORT = 37999;

  let mem = null, i32 = null, u8 = null, u32 = null, ws = null, connected = false;
  const stats = { tx: 0, rx: 0, dropRx: 0, txBytes: 0, rxBytes: 0 };

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
    i32 = new Int32Array(m.buffer);
    u8 = new Uint8Array(m.buffer);
    u32 = new Uint32Array(m.buffer);
    Atomics.store(i32, MAGIC >> 2, MAGIC_VALUE);
    console.log("[rkring] shared memory captured, " + m.buffer.byteLength + " B");
    connect();
    pumpLoop();
    flushLogs();
    status();
  }

  function connect() {
    ws = new WebSocket("ws://127.0.0.1:" + BRIDGE_PORT + "/");
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      connected = true;
      console.log("[rkring] bridge connected");
    };
    ws.onclose = () => {
      connected = false;
      setTimeout(connect, 1000);
    };
    ws.onerror = () => {};
    ws.onmessage = (ev) => enqueue(ev.data);
  }

  // ------------------------------------------------------------- the rings ---
  // Polling costs a wakeup per tick, so back off to 10 ms when there is nothing
  // to send and snap back to 2 ms the moment a datagram appears.
  let pumpDelay = 2;
  function pumpLoop() {
    const sent = connected ? pumpOut() : 0;
    pumpDelay = sent ? 2 : Math.min(10, pumpDelay + 1);
    setTimeout(pumpLoop, pumpDelay);
  }

  function pumpOut() {
    let tail = Atomics.load(i32, TX_TAIL >> 2);
    const head = Atomics.load(i32, TX_HEAD >> 2);
    let sent = 0;
    while (tail !== head) {
      const base = TX0 + (tail & (NSLOT - 1)) * SLOT;
      const len = u32[base >> 2];
      if (len === 0 || len > MAX_DATAGRAM) break;
      try {
        ws.send(u8.slice(base + 4, base + 4 + len));
      } catch (e) {
        break;
      }
      stats.tx++;
      stats.txBytes += len;
      sent++;
      tail++;
      Atomics.store(i32, TX_TAIL >> 2, tail);
    }
    return sent;
  }

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
    Atomics.notify(i32, WAKE >> 2, 1); // wake the engine's recv thread
    stats.rx++;
    stats.rxBytes += len;
  }

  // ---------------------------------------------------------------- status ---
  // The webview has no console a terminal can read, so the page forwards its
  // console and a counter snapshot to rkhost (/__log, /__client).
  const recent = [];
  let pending = [];

  function describe(x) {
    if (typeof x === "string") return x;
    if (x instanceof Error) return x.name + ": " + x.message;
    if (x && typeof x === "object") {
      try {
        const j = JSON.stringify(x);
        if (j && j !== "{}") return j;
      } catch (e) {}
      return String(x);
    }
    return String(x);
  }

  for (const k of ["log", "warn", "error", "info", "debug"]) {
    const orig = console[k].bind(console);
    console[k] = (...a) => {
      const line = k + "|" + a.map(describe).join(" ").slice(0, 500);
      recent.push(line);
      if (recent.length > 40) recent.shift();
      pending.push(line);
      if (pending.length > 300) pending.shift();
      orig(...a);
    };
  }

  function flushLogs() {
    if (pending.length) {
      const batch = pending;
      pending = [];
      try { fetch("/__log", { method: "POST", body: JSON.stringify(batch) }); } catch (e) {}
    }
    setTimeout(flushLogs, 1000);
  }

  function status() {
    const body = JSON.stringify({
      age: Math.round(performance.now()),
      coi: typeof crossOriginIsolated === "undefined" ? null : crossOriginIsolated,
      sab: typeof SharedArrayBuffer !== "undefined",
      jspi: typeof WebAssembly.Suspending === "function",
      stats: { ...stats },
      log: recent.slice(-10),
    });
    try { fetch("/__client", { method: "POST", body }); } catch (e) {}
    setTimeout(status, 3000);
  }
})();
