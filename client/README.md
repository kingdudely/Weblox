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

The client recovers from a stalled "Joining server" by restarting itself after
30 seconds without a rendered frame (`page/watchdog.ts`). Set
`WC_WATCHDOG_MS=0` to disable, or `WC_WATCHDOG_LOG=1` to see restarts.

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

## Debug

- `WC_INPUT=1` — log every event delivered to the engine, with the synthesized
  `keyCode`, `buttons`, and `movementX/Y` Deno omits
- `WC_SIZES=1` — log surface/canvas sizes when configuring and presenting
- `WC_SELFTEST=1` — inject one key/mouse/wheel sequence after 4 s

## Status

Join, rendering, and input work. Audio is still a stub (`AudioContext` with a
ScriptProcessor and no output device); it is kept in `page/env.ts` and will be
replaced with a real CPAL-backed `AudioContext` next.
