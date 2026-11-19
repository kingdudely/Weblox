Copyright notice, and the three host attempts

# alcohol

Run Roblox's web client on a native host, eventually for many accounts.

Three attempts live here, in the order they were made:

| dir | what it is | state |
| --- | --- | --- |
| `electron/` | Chromium window pointed at `play.roblox.com`, with the join-patch extension | **works** — joins, renders, input, leave |
| `v2-wasmtime/` | Rust host embedding the real `WebClient.wasm` under wasmtime | parked |
| `v3-wasm2c/` | wasm2c translation of the same module into C | parked experiment |

`electron` is the current one and the reference implementation: anything the
other hosts do differently is a host bug.

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

- Deno's own WebTransport client is not interoperable with Roblox's server: it
  never sends `sec-webtransport-http3-draft`, so the server answers with
  draft-02 stream framing, which Deno's reader parses as draft-07 and drops.
  A quinn-based client (`@moq/web-transport`) works.
- The public join gate is passed with `rbx-user-agent: Roblox/Darwin` plus
  `origin`/`referer: https://play.roblox.com`, and the browser host name is
  derived by incrementing the last octet of the native UDMUX address
  (`.33` → `.34`).

## Licensing

`electron/extension` is the MIT/Apache-2.0 licensed public-web-play-extension.
`@moq/web-transport` is used by the Deno-era notes only.
