# probe

CDP harness for driving the live engine. Everything here exists because the
engine is hostile to observation: it runs in page scope, fetches its module from
a CDN, and hides its transport behind a JS glue layer.

| file | purpose |
| --- | --- |
| `runprobe.js` | **main entry** — launches Chrome, injects the cookie, flips fast flags, redirects `WebClient.wasm` to a local patched module, logs console |
| `tapprobe.js` | same harness without the wasm redirect; used to observe the engine |
| `tap.js` | hook injected before the engine runs: wraps the `rbx_wasm_rna_*` imports, logs channel opens, datagram/stream payloads, receive-ring geometry, and heap-scans for string anchors |
| `flagtap.js` | rewrites the `WebClientApp` client-settings response in flight |

```sh
node --experimental-websocket tools/probe/runprobe.js \
  --place <id> --cookie /path/to/cookie.txt \
  --wasm-url http://127.0.0.1:8765/WebClient.wasm \
  --flags FFlagUseRbxTransportClient3=False \
  --seconds 70 --out /tmp/run.jsonl
```

Everything lands in `--out` as JSONL: `console`, `exc`, `target`, `wasm`, `done`.
Filter with `jq -r 'select(.k=="console")|.s'`.

## Learned the hard way

- The engine instantiates **in page scope**; the dozen workers are the pthread
  pool. Install hooks with `Page.addScriptToEvaluateOnNewDocument`, not per
  worker.
- `Runtime.runIfWaitingForDebugger` must be called for every attached target or
  the page hangs.
- Exported callbacks are re-wrapped by the glue (`Object.assign`), so wrapping
  `instance.exports` silently does nothing — tap the imports instead.
- The wasm redirect must keep `application/wasm` and carry CORS + CORP headers,
  because the page is cross-origin isolated.
- Fast flags are strings in the `WebClientApp` settings JSON, e.g.
  `FFlagUseRbxTransportClient3=False`.
