Copyright notice, and the three host attempts

# alcohol

Run Roblox's web client on a native host, eventually for many accounts.

Four attempts live here:

| dir | what it is | state |
| --- | --- | --- |
| `client/` | Deno raw backend (no DOM, WebGPU + WebTransport), wasm supplies the whole protocol | **primary product** — HTTP join works, but game server never opens data streams (see [investigation](client/INVESTIGATION.md)); low-RAM goal |
| `electron/` | Chromium window pointed at `play.roblox.com`, with the join-patch extension | **works** — joins, renders, input, leave. Reference implementation: it proves the engine/protocol itself is fine |
| `v2-wasmtime/` | Rust host embedding the real `WebClient.wasm` under wasmtime | parked |
| `v3-wasm2c/` | wasm2c translation of the same module into C | parked experiment |

**Read first:** [client/README.md](client/README.md) for how to run/build the
primary client, and [client/INVESTIGATION.md](client/INVESTIGATION.md) for the
full byte-level evidence on why it stalls and what has been ruled out. Starting
the Deno join from scratch without reading those will repeat already-failed
experiments.

Large generated artifacts (cached engine files, wasmtime compile cache, wasm2c
translation, captures) are **not** in git. See [REGENERATING.md](REGENERATING.md)
for how to recreate each one.

## Running the Electron version

```sh
cd electron
./run.sh 1818          # place id, or WC_PLACE=1818 ./run.sh
```

Needs a `.ROBLOSECURITY` value: `WC_COOKIE` inline, or `WC_COOKIE_FILE`
pointing at a file. Escape → Leave ends the run and closes the app.

## Why Electron

The web client needs WebGPU, WebTransport, JSPI, pointer lock and shared-memory
workers at once. Chromium has all of them; the other hosts have to supply them.

Cost per instance is roughly 4–5 per 8 GB, so density is the reason `v2`/`v3`
exist: they keep the same engine but replace the browser.

## Notes carried over from the earlier work

`docs/reference/` holds the original `WasmPlayer.js` / `WebClient.js` snapshots
and the join-route analysis. `docs/imports.tsv` and `docs/imports-wasm.json` are
the complete import surface of `WebClient.wasm` (module, name, wasm signature,
and the glue's JS signature where it has one) — the target list for any
non-browser host.

Two facts that cost a lot to learn and are worth keeping:

- The public join gate is passed with `rbx-user-agent: Roblox/Darwin` (the
  browser UA stays the Chrome UA). `Roblox/Web` returns `status 12` for public
  places. The browser host name (`gameFqdn`) is derived by incrementing the last
  octet of the native UDMUX address (`.33` → `.34`) when the server returns
  `gameFqdn: null`. See [client/README.md](client/README.md).
- The WebTransport/QUIC discrepancy is **not** just the missing `origin` header
  or a single npm library — four stacks (Deno/quinn, wtransport, libquiche,
  rwebtransport with BoringSSL) fail identically, and all observable
  protocol bytes match Chrome. See [client/INVESTIGATION.md](client/INVESTIGATION.md)
  before forming a new transport hypothesis.

## Licensing

`electron/extension` is the MIT/Apache-2.0 licensed public-web-play-extension.
`@moq/web-transport` is used by the Deno-era notes only.
