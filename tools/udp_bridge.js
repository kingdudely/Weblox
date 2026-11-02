#!/usr/bin/env node
/* udp_bridge.js — the real UDP socket for the patched Roblox web client.
 *
 * The engine talks RakNet, but its module was compiled without sockets: sendto,
 * recvfrom and select are a shared stub. tools/patch.py splices a pair of ring
 * buffers into the module, content.js pumps them over a WebSocket to this
 * process, and this process owns the dgram socket.
 *
 * Two modes:
 *
 *   --dest IP:PORT   fixed peer. Frames are raw datagrams in both directions.
 *                    This is the mode the RakNet transport uses: the engine's
 *                    getaddrinfo is synthetic, so the real server address has
 *                    to come from somewhere else.
 *
 *   (no --dest)      legacy mode: frames carry a routing header, so one bridge
 *                    can serve several peers. Big endian:
 *                      0     u8   type (0 = wasm -> host, 1 = host -> wasm)
 *                      1..4  u8x4 IPv4 octets
 *                      5..6  u16  port
 *                      7..        payload
 *
 * usage:
 *   node tools/udp_bridge.js --dest 128.116.47.34:49180 --verbose
 *   node tools/udp_bridge.js --port 37999
 */

const { WebSocketServer } = require("ws");
const dgram = require("dgram");

const args = process.argv.slice(2);
const getArg = (name, def) => {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : def;
};
const PORT = parseInt(getArg("--port", "37999"), 10);
const VERBOSE = args.includes("--verbose");
const DEST = getArg("--dest", null);
const DUMP = parseInt(getArg("--dump", "0"), 10);
let dumped = 0;

const stats = { tx: 0, rx: 0, txBytes: 0, rxBytes: 0, dropped: 0 };
const sockets = new Map(); // "ip:port" -> dgram socket
const clients = new Set();

function socketFor(ip, port) {
  const key = ip + ":" + port;
  const existing = sockets.get(key);
  if (existing) return existing;

  const s = dgram.createSocket("udp4");
  s.on("message", (msg, rinfo) => {
    stats.rx++;
    stats.rxBytes += msg.length;
    const frame = DEST
      ? msg
      : (() => {
          const out = Buffer.allocUnsafe(7 + msg.length);
          const oct = rinfo.address.split(".").map(Number);
          out[0] = 1;
          out[1] = oct[0];
          out[2] = oct[1];
          out[3] = oct[2];
          out[4] = oct[3];
          out[5] = rinfo.port >> 8;
          out[6] = rinfo.port & 0xff;
          msg.copy(out, 7);
          return out;
        })();
    for (const c of clients) {
      if (c.readyState !== 1) continue;
      try {
        c.send(frame);
      } catch (e) {
        stats.dropped++;
      }
    }
    if (VERBOSE) console.log(`rx ${msg.length}B <- ${rinfo.address}:${rinfo.port}`);
  });
  s.on("error", (e) => {
    console.log(`udp error ${key}: ${e.message}`);
    sockets.delete(key);
  });
  sockets.set(key, s);
  if (VERBOSE) console.log(`udp socket opened -> ${key}`);
  return s;
}

function sendOut(payload, ip, port) {
  if (!payload.length) return;
  stats.tx++;
  stats.txBytes += payload.length;
  const s = socketFor(ip, port);
  s.send(payload, port, ip, (err) => {
    if (err) {
      stats.dropped++;
      if (VERBOSE) console.log(`send ${ip}:${port} failed: ${err.message}`);
    }
  });
  if (VERBOSE) console.log(`tx ${payload.length}B -> ${ip}:${port}`);
  if (dumped < DUMP) {
    dumped++;
    console.log(`tx#${dumped} ${payload.length}B ${payload.subarray(0, 32).toString("hex")}`);
  }
}

let destIp = null;
let destPort = 0;
if (DEST) {
  const m = /^([\d.]+):(\d+)$/.exec(DEST);
  if (!m) {
    console.error(`bad --dest ${DEST}; want IP:PORT`);
    process.exit(2);
  }
  destIp = m[1];
  destPort = parseInt(m[2], 10);
}

const wss = new WebSocketServer({ port: PORT, host: "127.0.0.1" });
console.log(
  `bridge listening ws://127.0.0.1:${PORT}/` +
    (DEST ? ` -> ${destIp}:${destPort}` : " (routing header mode)")
);

wss.on("connection", (ws, req) => {
  console.log(`engine connected from ${req.socket.remoteAddress}`);
  clients.add(ws);

  ws.on("message", (buf, isBinary) => {
    if (!isBinary) {
      try {
        const m = JSON.parse(buf.toString());
        if (m.t === "dest" && /^[\d.]+:\d+$/.test(m.v || "")) {
          const [ip, port] = m.v.split(":");
          destIp = ip;
          destPort = parseInt(port, 10);
          console.log(`destination set -> ${destIp}:${destPort}`);
        }
      } catch (e) {}
      return;
    }
    if (destIp) {
      sendOut(buf, destIp, destPort);
      return;
    }
    if (buf.length < 7 || buf[0] !== 0) return;
    const ip = `${buf[1]}.${buf[2]}.${buf[3]}.${buf[4]}`;
    const port = (buf[5] << 8) | buf[6];
    sendOut(buf.subarray(7), ip, port);
  });

  ws.on("close", () => {
    console.log("engine disconnected");
    clients.delete(ws);
  });
});

if (process.env.RK_BRIDGE_STATS !== "0") {
  setInterval(() => {
    if (stats.tx || stats.rx || stats.dropped) {
      console.log(
        `stats tx=${stats.tx} (${stats.txBytes}B) rx=${stats.rx} (${stats.rxBytes}B) dropped=${stats.dropped}`
      );
    }
  }, 5000).unref();
}
