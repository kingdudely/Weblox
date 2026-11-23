# Alcohol — Roblox web client in a native window

The engine is Roblox's own web build. Deno supplies the browser-shaped page
(`page/env.ts`), the window and WebGPU surface (`main.ts`), and real threads for
Emscripten's pthreads (`page/worker.ts`). No Rust host, no Electron.

## Run

```sh
./run.sh 189707
```

Defaults: cookie from `/home/john/mizg.txt`, place `189707`. Override with
`WC_COOKIE_FILE`, `WC_PLACE`, or the first argument.

The client recovers from a stalled "Joining server" by restarting itself on a
separate thread (`page/watchdog.ts`): after 18 s if the gamejoin POST never
happens, or 45 s if it did but no frame appeared, and immediately on a GPU
device loss. Set `WC_WATCHDOG_MS=0` to disable all restarts (including device
loss), or `WC_WATCHDOG_LOG=1` to see them. Only one client should be joined at
a time — overlapping sessions on the same account get disconnected.

## Build

```sh
deno desktop -A --include page --include mirror --include page/watchdog.ts main.ts
```

`deno desktop` bundles **and runs** the app, so stop it (Ctrl-C) once `Bundle
client` appears. The binary lands in `client/client/`.

## Network / cache

Engine files and assets are fetched once and cached under `mirror/`. The build
version and the page's asset hash are detected from the real endpoints, so a
new Roblox build only needs `rm -rf mirror` (or `WC_REFRESH=1`) to be picked up.

- `WC_ENGINE` — pin the engine base URL
- `WC_PAGE_ASSETS` — pin the page asset prefix
- `WC_REFRESH=1` — re-resolve the build and re-download files
- `WC_MIRROR` — alternate mirror directory
- `WC_GLES=1` — force WebGL flags instead of WebGPU
- `WC_JOIN_UA` — the identity used for the gamejoin request. Both `user-agent`
  and `rbx-user-agent` are set to the same value (default
  `Roblox/Android/2.743.1136`), matching the native clients (Boblox/Rum). A
  Chrome `user-agent` paired with a `Roblox/*` `rbx-user-agent` is a
  contradiction and was associated with empty network tokens.
- `WC_RBX_UA` — legacy override for `rbx-user-agent` on non-join requests
  (unused for gamejoin now).

The gamejoin response is rewritten to add `gameFqdn` (the server returns
`null` for public places), and a response with an empty network token is
retried automatically up to 3 times. The engine's default loading screen is
dismissed once it reaches "Loading experience..." (`WC_FORCE_DISMISS=0` keeps
it).

## Debug

- `WC_INPUT=1` — log every event delivered to the engine, with the synthesized
  `keyCode`, `buttons`, and `movementX/Y` Deno omits
- `WC_SIZES=1` — log surface/canvas sizes when configuring and presenting
- `WC_SELFTEST=1` — inject one key/mouse/wheel sequence after 4 s
- `WC_JOIN_DEBUG=1` — log the fields of the gamejoin response, including the
  raw `gameFqdn` and both token fields
- `WC_NET_DEBUG=1` — log every fetch start/finish and worker `__ready`
- `WC_FORCE_DISMISS=0` — keep the engine's default loading screen
- `WC_DUMP=1` — capture the app's own rendered canvas to a local raw file
  (`WC_DUMP_FILE`, default `/tmp/opencode/appdump.png`). System/Wayland
  screenshot APIs are unavailable here; convert the raw BGRA dump with
  `python3 -c "from PIL import Image; d=open('$WC_DUMP_FILE','rb').read(); i=d.index(b'\\n'); w,h=map(int,d[:i].decode().split('x')); b=((w*4+255)//256)*256; Image.frombytes('RGBA',(w,h),d[i+1:],'raw','BGRA',b).save('/tmp/opencode/appview.png')"`

## Status

Join, rendering, and input work. Audio is still a stub (`AudioContext` with a
ScriptProcessor and no output device); it is kept in `page/env.ts` and will be
replaced with a real CPAL-backed `AudioContext` next.
