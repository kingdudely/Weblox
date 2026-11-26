# Regenerating generated artifacts

This repository ships **source only**. Large generated artifacts are downloaded
from Roblox's CDN on first run or produced locally, and are gitignored.

If you are an AI reading this: **do not try to "restore" these files from git.**
They were removed on purpose. Generate them with the steps below.

## `client/mirror/` — cached engine + page assets (gitignored)

The Deno client needs Roblox's own `WebClient.js`, `WebClient.wasm`, and asset
files. These are fetched from Roblox's CDN automatically on first run and
cached in `client/mirror/files/`:

```sh
cd client
deno task dev
```

On a cold start, `page/net.ts` resolves the current engine version from
`https://setup.rbxcdn.com/webclient/` and downloads everything referenced by
`WebClient.js` / `WasmPlayer.js`. The resolved version is pinned in
`client/mirror/engine.json`, so later runs are offline.

- Force a refresh against a new Roblox build: `rm -rf client/mirror` or set
  `WC_REFRESH=1`.
- Pin a specific build with `WC_ENGINE`, e.g.
  `https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient`.
- The known-good build at the time of writing is
  `version-1ef59424f70d4932-RobloxWebClient`.

A **reference copy** of the engine files is tracked under
`docs/reference/research/` (`WebClient.js`, `WebClient.wasm`, `WasmPlayer.js`,
`WasmLoader.js`) for study only — the client reads from `client/mirror/`.

## Built launcher (gitignored)

`client/client` and `client/client.so` are produced by Deno Desktop:

```sh
cd client
deno desktop -A --unstable-net --include page --include mirror main.ts
```

Stop when `Bundle client` appears. Alternatively, run from source with your own
Deno: `deno task dev`. Required unstable features are declared in
`client/deno.json` (`webgpu`, `worker-options`, `net`).

## Historical captures (gitignored)

`*.har`, `*.pcap`, and `*.log` are local evidence captures. Re-capture them if
needed; they are not part of the source distribution.

## Retired experiments

The wasmtime and wasm2c harnesses, their generated caches and translations, and
the associated regeneration notes were removed from `master`. Their source is
preserved on local branches:

| Branch | Contents |
| --- | --- |
| `experiment/wasmtime` | Rust/wasmtime host, import layer, WebTransport backend |
| `experiment/old-scheduler` | Earlier fibre scheduler |
| `experiment/wasm2c` | wasm2c translation harness and scripts |
| `experiment/socket-patch`, `experiment/udp-host`, `experiment/webtransport` | RakNet/WebTransport experiments |

See [docs/ARCHIVE.md](docs/ARCHIVE.md) for what each contains and why it was
retired.

## Summary

| Path | Tracked? | Recreate with |
|---|---|---|
| `client/mirror/**` | no | `cd client && deno task dev` (auto-download) |
| `client/client`, `client/*.so` | no | `deno desktop ...` (see above) |
| `*.pcap`, `*.har`, `*.log` | no | re-capture / re-export |
| All source, scripts and docs | yes | — |
