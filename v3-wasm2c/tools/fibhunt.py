#!/usr/bin/env python3
"""Attach to a Chrome's browser target and instrument WebLox's wasm fibre imports.

Every new page target gets a document-start script that wraps
WebAssembly.instantiate / instantiateStreaming and logs the
`__asyncjs__*fiber*` imports with their arguments and results.  That gives the
exact (from, to, ret) contract the engine expects.
"""
import asyncio, json, sys, time
import websockets

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9222
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/opencode/fiblog.txt"

INJECT = r"""
(() => {
  if (globalThis.__fibHook) return;
  globalThis.__fibHook = 1;
  const log = (...a) => { try { console.log('FIBLOG ' + a.join(' ')); } catch (e) {} };
  const wrap = (mod) => {
    if (!mod || !mod.env) return;
    const env = mod.env;
    for (const name of Object.keys(env)) {
      if (!/fiber|Futex|pthread_create|set_thread_state|stack_/.test(name)) continue;
      const fn = env[name];
      if (typeof fn !== 'function') continue;
      env[name] = function (...a) {
        if (name.includes('transfer')) log('XFER', a[0], '->', a[1]);
        else if (name.includes('create_suspended')) log('CREATE', a[0], a[1], a[2], a[3], a[4]);
        else log('CALL', name, a.join(','));
        let r;
        try { r = fn.apply(this, a); }
        catch (e) { log('THROW', name, String(e).slice(0, 80)); throw e; }
        if (name.includes('transfer')) log('XRET', a[0], '->', a[1], '=', r);
        else if (name.includes('create_suspended')) log('CRET', a[0], '=', r);
        return r;
      };
    }
  };
  const oi = WebAssembly.instantiate;
  WebAssembly.instantiate = async function (bytes, imports) { wrap(imports); return oi.call(this, bytes, imports); };
  const ois = WebAssembly.instantiateStreaming;
  if (ois) WebAssembly.instantiateStreaming = async function (src, imports) { wrap(imports); return ois.call(this, src, imports); };
  // the app's own frame-load timer is 30 s; stretch it so a slow guest start
  // does not abort the launch
  const st0 = globalThis.setTimeout;
  globalThis.setTimeout = function (f, d, ...a) { return st0(f, d === 30000 ? 120000 : d, ...a); };
  // a zero-sized controlledframe does not always start its guest; give the
  // hidden auth frame a real box (kept offscreen) so loadstop can fire
  try {
    const st = document.createElement('style');
    st.textContent = 'controlledframe#rbx-auth{display:block!important;' +
      'position:fixed!important;left:0!important;top:0!important;z-index:0!important;' +
      'width:1024px!important;height:768px!important;opacity:0!important;' +
      'pointer-events:none!important}';
    const add = () => (document.head || document.documentElement || document).appendChild(st);
    if (document.head || document.documentElement) add();
    else document.addEventListener('DOMContentLoaded', add);
    log('auth-frame visible');
  } catch (e) {}
  log('hooked');
})();
"""


async def main():
    base = f"http://127.0.0.1:{PORT}"
    import urllib.request
    ver = json.load(urllib.request.urlopen(base + "/json/version"))
    ws_url = ver["webSocketDebuggerUrl"]
    n = [0]
    out = open(OUT, "a", buffering=1)

    async with websockets.connect(ws_url, max_size=None) as ws:
        async def send(method, params=None, sid=None):
            n[0] += 1
            msg = {"id": n[0], "method": method, "params": params or {}}
            if sid:
                msg["sessionId"] = sid
            await ws.send(json.dumps(msg))
            return n[0]

        await send("Target.setDiscoverTargets", {"discover": True})
        await send("Target.setAutoAttach", {"autoAttach": True, "waitForDebuggerOnStart": True,
                                            "flatten": True})
        print("attached, waiting for pages", flush=True)
        while True:
            raw = await ws.recv()
            msg = json.loads(raw)
            m = msg.get("method")
            sid = msg.get("sessionId")
            if m == "Target.attachedToTarget":
                t = msg["params"]["targetInfo"]
                if t["type"] in ("page", "iframe", "worker", "app", "webview"):
                    s = msg["params"]["sessionId"]
                    await send("Runtime.enable", {}, s)
                    await send("Page.enable", {}, s)
                    await send("Log.enable", {}, s)
                    if t["type"] == "worker":
                        await send("Runtime.evaluate", {"expression": INJECT}, s)
                    else:
                        await send("Page.addScriptToEvaluateOnNewDocument", {"source": INJECT}, s)
                    await send("Runtime.runIfWaitingForDebugger", {}, s)
                    print("injected into", t["type"], t["url"][:70], flush=True)
            elif m == "Runtime.consoleAPICalled":
                args = []
                for a in msg["params"].get("args", []):
                    v = a.get("value")
                    if v is None:
                        v = a.get("description") or a.get("type")
                    args.append(str(v))
                line = " ".join(args)
                out.write(("FIB " if line.startswith("FIBLOG") else "LOG ") + line[:400] + "\n")
            elif m == "Target.detachedFromTarget":
                pass


asyncio.run(main())
