# probe

CDP harness for the live engine. Launches real Chrome (headed, GPU), injects
`run/cookie.txt`-style `.ROBLOSECURITY`, attaches to every worker *before* its
script runs, and taps the engine at the `WebAssembly.instantiate` boundary.

```sh
node --experimental-websocket tools/probe/tapprobe.js \
  --url 'https://play.roblox.com/games/start?placeId=<id>' \
  --cookie /path/to/cookie.txt \
  --hook tools/probe/flagtap.js \
  --seconds 70 --out /tmp/run.jsonl
```

- `tap.js` — wraps the `rbx_wasm_rna_*` imports and the exported callbacks:
  logs channel opens, datagram/stream payload hex, and the receive-ring
  geometry (`data/head/tail/slotSize/slotCount`). Also heap-scans for string
  anchors and prints their linear addresses.
- `flagtap.js` — rewrites the `WebClientApp` client-settings response in flight
  to flip fast flags (`FFlagUseRbxTransportClient3=False` selects RakNet).

Notes learned the hard way:

- The engine instantiates **in page scope**; the workers are the pthread pool.
  Install hooks with `Page.addScriptToEvaluateOnNewDocument`, not per-worker.
- `Runtime.runIfWaitingForDebugger` must be called for every attached target or
  the page hangs.
- The exported callbacks (`rbx_wasm_rna_on_*`) are re-wrapped by the glue via
  `Object.assign`, so wrapping `instance.exports` is unreliable — tap imports.
