import asyncio, json, urllib.request, sys, base64
import websockets

async def main():
    path, out = sys.argv[1], sys.argv[2]
    tl = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json/list"))
    app = [t for t in tl if t["url"].startswith("isolated-app://")]
    if not app:
        sys.exit("no app target")
    async with websockets.connect(app[0]["webSocketDebuggerUrl"], max_size=None) as ws:
        expr = ("fetch(%s).then(r=>r.arrayBuffer()).then(b=>{"
                "let s='';const u=new Uint8Array(b);for(let i=0;i<u.length;i+=0x8000)"
                "s+=String.fromCharCode.apply(null,u.subarray(i,i+0x8000));"
                "return btoa(s)})" % json.dumps(path))
        await ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                                  "params": {"expression": expr, "awaitPromise": True,
                                             "returnByValue": True}}))
        while True:
            m = json.loads(await ws.recv())
            if m.get("id") == 1:
                r = m.get("result", {})
                if "exceptionDetails" in r:
                    sys.exit(str(r["exceptionDetails"])[:300])
                open(out, "wb").write(base64.b64decode(r["result"]["value"]))
                print("wrote", out)
                return

asyncio.run(main())
