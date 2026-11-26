// @ts-nocheck
/**
 * WebTransport draft-02 over Deno's raw QUIC API (--unstable-net).
 *
 * Deno's built-in WebTransport has no `headers` option in its WebIDL converter,
 * so it can never send `sec-webtransport-http3-draft02: 1`, which Roblox/qdmux
 * requires (Chrome NetLog: webtransport_http3_version=draft-02). This module
 * reimplements the small H3/WebTransport layer from web-transport-proto 0.2.7
 * in TS, adding that header. Stream/datagram framing is from Deno's own
 * ext/web/webtransport.js.
 */

const enc = new TextEncoder();
const dec = new TextDecoder();

const STREAM_CONTROL = 0x00n;
const STREAM_QPACK_ENC = 0x02n;
const STREAM_QPACK_DEC = 0x03n;
const WT_UNI = 0x54n;
const WT_BI = 0x41n;
const FRAME_HEADERS = 0x01n;
const FRAME_SETTINGS = 0x04n;

const SETTINGS = [
  [0x08n, 1n], [0x33n, 1n], [0xffd277n, 1n],
  [0xc671706an, 1n], [0x2b603743n, 1n], [0x2b603742n, 1n],
];

const STATIC: Array<[string, string]> = [
  [":authority", ""], [":path", "/"], ["age", "0"],
  ["content-disposition", ""], ["content-length", "0"], ["cookie", ""],
  ["date", ""], ["etag", ""], ["if-modified-since", ""], ["if-none-match", ""],
  ["last-modified", ""], ["link", ""], ["location", ""], ["referer", ""],
  ["set-cookie", ""], [":method", "CONNECT"], [":method", "DELETE"],
  [":method", "GET"], [":method", "HEAD"], [":method", "OPTIONS"],
  [":method", "POST"], [":method", "PUT"], [":scheme", "http"],
  [":scheme", "https"], [":status", "103"], [":status", "200"],
  [":status", "304"], [":status", "404"], [":status", "503"],
];

function varint(v) {
  v = BigInt(v);
  if (v < 64n) return Uint8Array.of(Number(v));
  if (v < 16384n) {
    const x = Number((0b01n << 14n) | v);
    return Uint8Array.of(x >> 8, x & 255);
  }
  if (v < 1073741824n) {
    const x = (0b10n << 30n) | v;
    return Uint8Array.of(Number(x >> 24n & 255n), Number(x >> 16n & 255n), Number(x >> 8n & 255n), Number(x & 255n));
  }
  let x = (0b11n << 62n) | v;
  const out = new Uint8Array(8);
  for (let i = 7; i >= 0; i--) { out[i] = Number(x & 255n); x >>= 8n; }
  return out;
}

function decodeVarint(data, offset) {
  const b = data[offset];
  if (b === undefined) throw new Error("short varint");
  const tag = b >> 6;
  const size = 1 << tag;
  let v = BigInt(b & (tag === 3 ? 63 : (1 << (6 - tag * 2)) - 1));
  for (let i = 1; i < size; i++) {
    if (data[offset + i] === undefined) throw new Error("short varint");
    v = (v << 8n) | BigInt(data[offset + i]);
  }
  return { value: v, size };
}

function prefix(size, flags, value) {
  const mask = (1 << size) - 1;
  if (value < mask) return [((flags << size) & 255) | value];
  const out = [((flags << size) & 255) | mask];
  let rest = value - mask;
  while (rest >= 128) { out.push((rest % 128) + 128); rest = Math.floor(rest / 128); }
  out.push(rest);
  return out;
}

function readPrefix(data, offset, size) {
  const mask = (1 << size) - 1;
  let first = data[offset];
  if (first === undefined) throw new Error("short prefix");
  const flags = first >> size;
  let value = first & mask;
  let i = offset + 1;
  if (value === mask) {
    let power = 0;
    while (true) {
      const b = data[i++];
      if (b === undefined) throw new Error("short prefix");
      value += (b & 127) * 2 ** power;
      power += 7;
      if (!(b & 128)) break;
      if (power > 70) throw new Error("prefix too long");
    }
  }
  return { flags, value, next: i };
}

function stringField(name, value) {
  const n = enc.encode(name), v = enc.encode(value);
  return [...prefix(3, 0b00100, n.length), ...n, ...prefix(7, 0, v.length), ...v];
}

function encodeHeaders(fields) {
  const out = [...prefix(8, 0, 0), ...prefix(7, 0, 0)];
  const entries = Object.entries(fields);
  entries.sort((a, b) => Number(!a[0].startsWith(":")) - Number(!b[0].startsWith(":")));
  for (const [k, v] of entries) out.push(...stringField(k, v));
  return Uint8Array.from(out);
}

function decodeHeaders(buf) {
  let i = 0;
  try { i = readPrefix(buf, i, 8).next; i = readPrefix(buf, i, 7).next; } catch { return {}; }
  const out = {};
  while (i < buf.length) {
    try {
      const peek = buf[i];
      if ((peek & 0xc0) === 0xc0) {
        const idx = readPrefix(buf, i, 6);
        const e = STATIC[idx.value];
        if (e) out[e[0]] = e[1];
        i = idx.next;
      } else if ((peek & 0xe0) === 0x20) {
        const nl = readPrefix(buf, i, 3);
        const name = dec.decode(buf.subarray(nl.next, nl.next + nl.value));
        const vl = readPrefix(buf, nl.next + nl.value, 7);
        out[name] = dec.decode(buf.subarray(vl.next, vl.next + vl.value));
        i = vl.next + vl.value;
      } else if ((peek & 0xf0) === 0x50) {
        const ni = readPrefix(buf, i, 4);
        const vl = readPrefix(buf, ni.next, 7);
        const e = STATIC[ni.value];
        out[e ? e[0] : "?"] = dec.decode(buf.subarray(vl.next, vl.next + vl.value));
        i = vl.next + vl.value;
      } else break;
    } catch { break; }
  }
  return out;
}

function frame(type, body) {
  return Uint8Array.from([...varint(type), ...varint(body.length), ...body]);
}

function settingsFrame() {
  const body = [];
  for (const [k, v] of SETTINGS) body.push(...varint(k), ...varint(v));
  return frame(FRAME_SETTINGS, Uint8Array.from(body));
}

/** Buffered reader over a QUIC stream. No byte is ever lost. */
class BufReader {
  constructor(stream) { this.r = stream.getReader(); this.buf = new Uint8Array(0); }
  async take(n) {
    let spins = 0;
    while (this.buf.length < n) {
      const { value, done } = await this.r.read();
      if (done) throw new Error("stream closed");
      if (!value || value.byteLength === 0) {
        if (++spins > 16) throw new Error("no progress reading stream");
        continue;
      }
      const next = new Uint8Array(this.buf.length + value.byteLength);
      next.set(this.buf); next.set(value, this.buf.length);
      this.buf = next;
      spins = 0;
    }
    const out = this.buf.subarray(0, n);
    this.buf = this.buf.subarray(n);
    return out;
  }
  releaseLock() { try { this.r.releaseLock(); } catch {} }

  /** ReadableStream that first yields bytes already buffered, then the rest. */
  asReadable() {
    const br = this;
    return new ReadableStream({
      async pull(controller) {
        if (br.buf.length) {
          const b = br.buf; br.buf = new Uint8Array(0);
          controller.enqueue(b);
          return;
        }
        const { value, done } = await br.r.read();
        if (done) controller.close();
        else if (value && value.byteLength) controller.enqueue(value);
      },
      cancel() { br.releaseLock(); },
    });
  }
}

async function readVarintBuf(br) {
  const first = await br.take(1);
  const tag = first[0] >> 6;
  const size = 1 << tag;
  if (size === 1) return BigInt(first[0] & (tag === 3 ? 63 : 63));
  const rest = await br.take(size - 1);
  const all = new Uint8Array(size); all[0] = first[0]; all.set(rest, 1);
  return decodeVarint(all, 0).value;
}

async function readFrame(br) {
  const type = await readVarintBuf(br);
  const len = await readVarintBuf(br);
  const body = await br.take(Number(len));
  return { type, body };
}

function wrapWritable(stream) {
  const w = stream.getWriter();
  let closed = false;
  return new WritableStream({
    async write(chunk) { if (!closed) await w.write(chunk); },
    async close() { if (!closed) { closed = true; await w.close(); w.releaseLock(); } },
    async abort() { if (!closed) { closed = true; try { await w.abort(); } catch {} w.releaseLock(); } },
  });
}

// Diagnostics for the transport-only disconnect seen after ~2 min of play.
// Verbose traces are opt-in with WC_WT_DBG=1; the QUIC close reason is always
// logged once when conn.closed settles.
const wtDbg = (() => {
  try { return Deno.env.get("WC_WT_DBG") === "1"; } catch { return false; }
})();
const wtEnvNum = (name, fallback) => {
  try {
    const v = Number(Deno.env.get(name));
    return Number.isFinite(v) && v > 0 ? v : fallback;
  } catch { return fallback; }
};
const wtLog = (...args) => { if (wtDbg) console.error("[draft02:dbg]", ...args); };
function wtDescribe(v) {
  if (v === undefined) return "undefined";
  if (v === null) return "null";
  if (typeof v !== "object") return String(v);
  const bits = [];
  for (const k of ["closeCode", "reason", "reasonPhrase", "code", "message", "name"]) {
    if (v[k] !== undefined) bits.push(`${k}=${String(v[k])}`);
  }
  if (!bits.length) {
    try { bits.push(JSON.stringify(v, (_, x) => typeof x === "bigint" ? String(x) : x)); }
    catch { bits.push(String(v)); }
  }
  if (v.stack) bits.push("stack=" + String(v.stack).slice(0, 400));
  return bits.join(" ");
}

export class DraftWebTransport {
  ready; closed;
  #conn; #sessionId = 0n; #readyResolve; #closedResolve; #reject;
  #headerUni = new Uint8Array(0); #headerBi = new Uint8Array(0);
  #dgIn = []; #dgWaiters = []; #dgUnknownLogged = false;
  #incomingUni = []; #incomingBi = [];
  #uniWake; #biWake;
  #maxDG = 1200;

  constructor(url, options = {}) {
    console.error("[draft02] constructed", String(url).slice(0, 100));
    const t0 = Date.now();
    const elapsed = () => ((Date.now() - t0) / 1000).toFixed(1) + "s";
    // Defaults unchanged; env overrides let us A/B test keepalive without a rebuild.
    const keepAliveInterval = wtEnvNum("WC_WT_KEEPALIVE_MS", 4e3);
    const maxIdleTimeout = wtEnvNum("WC_WT_IDLE_MS", 10e3);
    wtLog("connect options", { keepAliveInterval, maxIdleTimeout, congestionControl: options.congestionControl ?? "default" });
    this.ready = new Promise((r, x) => { this.#readyResolve = r; this.#reject = x; });
    this.closed = new Promise((r) => { this.#closedResolve = r; });
    const u = new URL(url);
    const pumpExit = (name, why, detail) => {
      console.error(`[draft02] ${name} pump EXIT`, "elapsed", elapsed(), "why", why,
        detail === undefined ? "" : wtDescribe(detail));
    };
    const doConnect = async () => {
      const conn = await (Deno as any).connectQuic({
        hostname: u.hostname, port: Number(u.port) || 443,
        alpnProtocols: ["h3"],
        serverCertificateHashes: options.serverCertificateHashes,
        keepAliveInterval, maxIdleTimeout,
        congestionControl: options.congestionControl ?? "default",
      });
      wtLog("connectQuic returned", elapsed());
      this.#conn = conn;
      conn.closed.then(
        (v) => { console.error("[draft02] conn.closed RESOLVED", "elapsed", elapsed(), "value", wtDescribe(v)); this.#closedResolve(v); },
        (e) => { console.error("[draft02] conn.closed REJECTED", "elapsed", elapsed(), "reason", wtDescribe(e)); this.#closedResolve(null); },
      );

      // control stream: SETTINGS
      const ctrl = await conn.createUnidirectionalStream();
      const cw = ctrl.getWriter();
      await cw.write(Uint8Array.from([...varint(STREAM_CONTROL), ...settingsFrame()]));
      cw.releaseLock();

      // Extended CONNECT with the draft-02 offer header
      const bidi = await conn.createBidirectionalStream();
      const headers = encodeHeaders({
        ":method": "CONNECT", ":scheme": "https", ":authority": u.host,
        ":path": u.pathname + u.search, ":protocol": "webtransport",
        "sec-webtransport-http3-draft02": "1",
      });
      const bw = bidi.writable.getWriter();
      await bw.write(frame(FRAME_HEADERS, headers));
      bw.releaseLock();
      const resp = await readFrame(new BufReader(bidi.readable));
      const rh = decodeHeaders(resp.body);
      console.error("[draft02] CONNECT", JSON.stringify(rh));
      if (!String(rh[":status"] ?? "").startsWith("2")) throw new Error("CONNECT " + rh[":status"]);

      this.#sessionId = BigInt(bidi.writable.id ?? 0n);
      const sidBuf = varint(this.#sessionId);
      this.#headerUni = Uint8Array.from([...varint(WT_UNI), ...sidBuf]);
      this.#headerBi = Uint8Array.from([...varint(WT_BI), ...sidBuf]);
      this.#maxDG = Math.max(0, conn.maxDatagramSize - sidBuf.length);
      console.error("[draft02] sessionId", String(this.#sessionId), "maxDG", this.#maxDG);
      this.#readyResolve();

      // ONE reader for all incoming uni streams: control / QPACK / WT.
      (async () => {
        try {
          const r = conn.incomingUnidirectionalStreams.getReader();
          wtLog("uni pump start", elapsed());
          while (true) {
            const { value: stream, done } = await r.read();
            if (done) { pumpExit("uni", "reader done"); return; }
            try {
              const br = new BufReader(stream);
              const type = await readVarintBuf(br);
              if (type === STREAM_CONTROL) {
                const f = await readFrame(br);
                console.error("[draft02] server settings frame", String(f.type));
                br.releaseLock();
              } else if (type === STREAM_QPACK_ENC || type === STREAM_QPACK_DEC) {
                br.releaseLock(); // hold; engine never uses them
              } else if (type === WT_UNI) {
                const sid = await readVarintBuf(br);
                console.error("[draft02] uni stream", String(type), String(sid), "ours", String(this.#sessionId));
                if (sid === this.#sessionId) {
                  console.error("[draft02] INCOMING STREAM ACCEPTED");
                  this.#incomingUni.push(br.asReadable()); this.#uniWake?.();
                } else br.releaseLock();
              } else {
                console.error("[draft02] uni unknown type", String(type));
                br.releaseLock();
              }
            } catch (e) { console.error("[draft02] uni stream err", e?.stack?.slice?.(0, 300) ?? String(e)); }
          }
        } catch (e) {
          pumpExit("uni", "read rejected", e);
        }
      })().catch((e) => console.error("[draft02] uni pump failed", "elapsed", elapsed(), wtDescribe(e)));

      (async () => {
        try {
          const r = conn.incomingBidirectionalStreams.getReader();
          wtLog("bi pump start", elapsed());
          while (true) {
            const { value: stream, done } = await r.read();
            if (done) { pumpExit("bi", "reader done"); return; }
            try {
              const br = new BufReader(stream.readable);
              const type = await readVarintBuf(br);
              const sid = await readVarintBuf(br);
              console.error("[draft02] bi stream", String(type), String(sid));
              if (type === WT_BI && sid === this.#sessionId) {
                this.#incomingBi.push({ readable: br.asReadable(), writable: stream.writable });
                this.#biWake?.();
              }
            } catch (e) { console.error("[draft02] bi stream err", String(e).slice(0, 200)); }
          }
        } catch (e) {
          pumpExit("bi", "read rejected", e);
        }
      })().catch((e) => console.error("[draft02] bi pump failed", "elapsed", elapsed(), wtDescribe(e)));

      // datagrams
      (async () => {
        const sid = varint(this.#sessionId);
        while (true) {
          try {
            const dg = await conn.readDatagram();
            if (dg.length >= sid.length && dg.subarray(0, sid.length).every((b, i) => b === sid[i])) {
              const body = dg.subarray(sid.length);
              if (this.#dgWaiters.length) this.#dgWaiters.shift()(body);
              else this.#dgIn.push(body);
            } else if (!this.#dgUnknownLogged) {
              this.#dgUnknownLogged = true;
              console.error("[draft02] unknown datagram prefix", Array.from(dg.slice(0, 8)).map((b) => b.toString(16).padStart(2, "0")).join(" "));
            }
          } catch { break; }
        }
      })().catch(() => {});

      return conn;
    };
    this._promise = doConnect().catch((e) => { this.#reject(e); console.error("[draft02] connect failed", e?.stack?.slice?.(0, 400) ?? String(e)); throw e; });
    this._promise.catch(() => this.#closedResolve(null));
  }

  getStats() { return Promise.resolve({}); }

  get datagrams() {
    const self = this;
    return {
      get maxDatagramSize() { return self.#maxDG; },
      readable: {
        getReader: () => ({
          read: async () => {
            if (self.#dgIn.length) return { value: self.#dgIn.shift(), done: false };
            const v = await new Promise((r) => self.#dgWaiters.push(r));
            return { value: v, done: false };
          },
          releaseLock() {},
        }),
      },
      writable: {
        getWriter: () => ({
          write: async (b) => {
            const conn = await self._promise;
            await conn.sendDatagram(Uint8Array.from([...varint(self.#sessionId), ...b]));
          },
          releaseLock() {},
        }),
      },
    };
  }

  get incomingUnidirectionalStreams() {
    const self = this;
    return {
      getReader: () => ({
        read: async () => {
          while (!self.#incomingUni.length) await new Promise((r) => { self.#uniWake = r; });
          return { value: self.#incomingUni.shift(), done: false };
        },
        releaseLock() {},
      }),
    };
  }

  get incomingBidirectionalStreams() {
    const self = this;
    return {
      getReader: () => ({
        read: async () => {
          while (!self.#incomingBi.length) await new Promise((r) => { self.#biWake = r; });
          return { value: self.#incomingBi.shift(), done: false };
        },
        releaseLock() {},
      }),
    };
  }

  async createUnidirectionalStream() {
    const conn = await this._promise;
    const s = await conn.createUnidirectionalStream();
    const w = s.getWriter();
    await w.write(this.#headerUni);
    w.releaseLock();
    return wrapWritable(s);
  }

  async createBidirectionalStream() {
    const conn = await this._promise;
    const s = await conn.createBidirectionalStream();
    const w = s.writable.getWriter();
    await w.write(this.#headerBi);
    w.releaseLock();
    return { readable: s.readable, writable: wrapWritable(s.writable) };
  }

  close(info = {}) {
    this.#conn?.close({ closeCode: info.closeCode ?? 0, reason: info.reason ?? "" });
  }
}

export default DraftWebTransport;
