# alcohol

Run Roblox's web client in a native window on a raw Deno host (no DOM, no
Electron). Roblox's own `WebClient.wasm` generates the entire protocol; this
repo only supplies the window, WebGPU surface, threads, and browser-shaped shims.

## Run

```sh
cd client
WC_COOKIE_FILE=/path/to/cookie.txt ./client/client 189707
```

Defaults: cookie `/home/john/mizg.txt`, place `189707` (both are in `main.ts`).
The `jebus` account cookie is known-good for place 189707:
`/tmp/opencode/jebus.txt`. Only one client should be joined at a time.

The binary must be built with `--unstable-net` so `Deno.connectQuic` is baked
in. See [client/README.md](client/README.md) for the build command.

## Layout

| dir | what it is |
| --- | --- |
| `client/` | **the product** — Deno raw host, WebGPU + draft-02 WebTransport/QUIC |
| `v2-wasmtime/` | parked research host: real wasm under wasmtime |
| `v3-wasm2c/` | parked research host: wasm2c translation of the same module |
| `docs/` | original engine snapshots, import surface, native RE notes |

Generated artifacts (engine mirror, wasmtime cache, wasm2c translation,
captures) are **not** in git. [REGENERATING.md](REGENERATING.md) says how to
recreate each. [client/INVESTIGATION.md](client/INVESTIGATION.md) is the
pre-fix investigation record — keep it, but do not re-run its ruled-out
hypotheses.

## Two facts worth keeping

- Public places require `rbx-user-agent: Roblox/Darwin` on the join POST
  (`Roblox/Web` gets HTTP 200 / `status: 12`, "Not authorized"). The browser UA
  stays Chrome. When the server returns `gameFqdn: null`, the browser route is
  derived from the token/IP/port with the UDMUX last octet +1.
- Roblox/qdmux uses **WebTransport draft-02**. Deno's built-in WebTransport
  cannot send the `sec-webtransport-http3-draft02: 1` CONNECT header, so
  `client/page/draftwt.ts` implements that header over raw QUIC. All four
  alternative transport libraries tried during the investigation failed
  identically — do not re-add them.
