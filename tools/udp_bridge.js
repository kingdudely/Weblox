#!/usr/bin/env node
/* udp_bridge.js — local UDP socket for the patched Roblox web client.
 *
 * The engine runs in a browser worker and cannot open a UDP socket, so the
 * glue (tools/patch_glue.py) forwards every datagram over a WebSocket to this
 * process, which owns one real dgram socket per peer address.
 *
 * Frame format (both directions), big endian:
 *   0        u8   type (0 = wasm -> host, 1 = host -> wasm)
 *   1..4     u8x4 IPv4 octets
 *   5..6     u16  port
 *   7..      payload
 *
 * usage: node tools/udp_bridge.js [--port 37999] [--verbose]
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

const sockets = new Map();          // "ip:port" -> dgram socket
let client = null;                  // single engine client
const stats = { tx: 0, rx: 0, txBytes: 0, rxBytes: 0, dropped: 0 };

function socketFor(ip, port) {
  const key = ip + ":" + port;
  let s = sockets.get(key);
  if (s) return s;

  s = dgram.createSocket("udp4");
  s.on("message", (msg, rinfo) => {
    stats.rx++;
    stats.rxBytes += msg.length;
    if (!client || client.readyState !== 1) {
      stats.dropped++;
      return;
    }
    const out = Buffer.allocUnsafe(7 + msg.length);
    out[0] = 1;
    out[1] = rinfo.address.split(".").map(Number)[0] || 0;
    const oct = rinfo.address.split(".").map(Number);
    out[1] = oct[0]; out[2] = oct[1]; out[3] = oct[2]; out[4] = oct[3];
    out[5] = rinfo.port >> 8;
    out[6] = rinfo.port & 0xff;
    msg.copy(out, 7);
    try {
      client.send(out);
    } catch (e) {
      stats.dropped++;
    }
    if (VERBOSE) console.log(`rx ${msg.length}B <- ${rinfo.address}:${rinfo.port}`);
  });
  s.on("error", (e) => {
    console.log(`udp error ${ip}:${port}: ${e.message}`);
    sockets.delete(key);
  });
  sockets.set(key, s);
  console.log(`udp socket opened -> ${ip}:${port}`);
  return s;
}

const wss = new WebSocketServer({ port: PORT, host: "127.0.0.1" });
console.log(`bridge listening ws://127.0.0.1:${PORT}/`);

wss.on("connection", (ws, req) => {
  console.log(`engine connected from ${req.socket.remoteAddress}`);
  client = ws;
  ws.on("message", (buf) => {
    if (buf.length < 7) return;
    const type = buf[0];
    if (type !== 0) return;
    const ip = `${buf[1]}.${buf[2]}.${buf[3]}.${buf[4]}`;
    const port = (buf[5] << 8) | buf[6];
    const payload = buf.subarray(7);
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
  });
  ws.on("close", () => {
    console.log("engine disconnected");
    if (client === ws) client = null;
  });
});

setInterval(() => {
  if (stats.tx || stats.rx) {
    console.log(
      `stats tx=${stats.tx} (${stats.txBytes}B) rx=${stats.rx} (${stats.rxBytes}B) dropped=${stats.dropped}`
    );
  }
}, 5000).unref();
