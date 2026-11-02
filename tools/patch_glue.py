#!/usr/bin/env python3
"""patch_glue.py — give the engine glue a UDP bridge.

The wasm module has no socket layer (every libc socket entry is a stub that
returns -1). Two unused imports were therefore repurposed in place by
tools/patch.py:

    import[202]  rbx_wasm_udp_send_js(sockaddr_ptr, buf, len) -> bytes| -1
    import[201]  rbx_wasm_udp_recv_js(sockaddr_out, buf, len) -> bytes| -1

This script patches WebClient.js so those imports exist and forward datagrams to
a local WebSocket bridge (tools/udp_bridge.js), which owns the real UDP socket.
The engine never sees a difference: it hands over a sockaddr and a buffer, the
glue reads/writes linear memory, and the bridge does connect/send/recv.

usage:
    patch_glue.py WebClient.js WebClient.patched.js --port 37999
"""

import argparse
import sys

# Anchor: the import object literal is a flat `key:value` map inside the glue
# factory, so `Ce` (the WebAssembly.Memory binding) is in scope for closures
# defined there. That is why the shims are inline arrows and not references.
IMPORT_ANCHOR = "emscripten_websocket_send_binary:fn,"

SHIM = r"""
// ---- raknet udp bridge -----------------------------------------------------
// Injected by tools/patch_glue.py. The engine calls these instead of real
// sendto()/recvfrom(); the host bridge owns the UDP socket.
globalThis.__rkUdp = (function () {
  const WS_URL = "__WS_URL__";
  let ws = null, ready = false, heap = null, heapBuf = null;
  const queue = [];              // datagrams waiting for a recv() call
  const maxQueue = 4096;

  function mem(m) {
    if (m && m.buffer !== heapBuf) {
      heapBuf = m.buffer;
      heap = new Uint8Array(heapBuf);
      globalThis.__rkHeap = heap;
    }
    return heap;
  }

  function connect() {
    if (ws) return;
    try {
      ws = new WebSocket(WS_URL);
      ws.binaryType = "arraybuffer";
      ws.onopen = () => {
        ready = true;
        console.log("[rkudp] bridge connected " + WS_URL);
      };
      ws.onclose = () => {
        ready = false;
        ws = null;
        setTimeout(connect, 500);
      };
      ws.onerror = () => { /* onclose follows */ };
      ws.onmessage = (ev) => {
        const b = new Uint8Array(ev.data);
        // frame: type(1) ip(4 BE) port(2 BE) payload
        if (b.length < 7) return;
        if (queue.length >= maxQueue) queue.shift();
        queue.push(b.slice(7));
        globalThis.__rkLast = { ip: [b[1], b[2], b[3], b[4]], port: (b[5] << 8) | b[6] };
      };
    } catch (e) {
      console.log("[rkudp] bridge error: " + (e && e.message));
      ws = null;
    }
  }
  connect();

  // sockaddr_in in linear memory: family u16 @0, port u16 @2 (network order),
  // addr u32 @4 (network order).
  function readAddr(h, p) {
    const port = (h[p + 2] << 8) | h[p + 3];
    return [h[p + 4], h[p + 5], h[p + 6], h[p + 7], port];
  }

  return function (isSend, m, a, b, c) {
    const h = mem(m);
    if (!h) return -1;

    if (isSend) {
      // a = sockaddr, b = payload, c = len
      if (!ready || c <= 0 || c > 65535) return -1;
      const [i0, i1, i2, i3, port] = readAddr(h, a);
      const frame = new Uint8Array(7 + c);
      frame[0] = 0;
      frame[1] = i0; frame[2] = i1; frame[3] = i2; frame[4] = i3;
      frame[5] = port >> 8; frame[6] = port & 255;
      frame.set(h.subarray(b, b + c), 7);
      try {
        ws.send(frame);
        return c;
      } catch (e) {
        return -1;
      }
    }

    // recv: a = sockaddr out, b = buffer, c = capacity
    if (!queue.length) return -1;
    const dg = queue.shift();
    const n = Math.min(dg.length, c);
    h.set(dg.subarray(0, n), b);
    // fill the out-address from the last seen source (single-server client)
    const last = globalThis.__rkLast || { ip: [0, 0, 0, 0], port: 0 };
    h[a] = 2; h[a + 1] = 0;
    h[a + 2] = last.port >> 8; h[a + 3] = last.port & 255;
    h[a + 4] = last.ip[0]; h[a + 5] = last.ip[1];
    h[a + 6] = last.ip[2]; h[a + 7] = last.ip[3];
    return n;
  };
})();
// ---- end raknet udp bridge -------------------------------------------------
"""


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--port", default="37999")
    a = ap.parse_args(argv[1:])

    js = open(a.src, "r", encoding="utf-8").read()
    if "emscripten_websocket_send_binary:fn," not in js:
        raise SystemExit("import anchor not found — glue changed, re-anchor")

    shim = SHIM.replace("__WS_URL__", "ws://127.0.0.1:%s/" % a.port)
    hooks = (
        "rbx_wasm_udp_send_js:(a,b,c)=>globalThis.__rkUdp(1,Ce,a,b,c),"
        "rbx_wasm_udp_recv_js:(a,b,c)=>globalThis.__rkUdp(0,Ce,a,b,c),"
    )
    js = js.replace(IMPORT_ANCHOR, IMPORT_ANCHOR + hooks, 1)

    # The shim must run before the factory body evaluates the import literal.
    js = shim + "\n" + js
    open(a.dst, "w", encoding="utf-8").write(js)
    print("wrote %s (%d bytes, bridge %s)" % (a.dst, len(js), "ws://127.0.0.1:%s/" % a.port))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
