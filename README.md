# alcohol

Run Roblox's web client in a native window on a raw Deno host — no DOM, no
Electron, no browser. Roblox's own `WebClient.wasm` generates the entire
protocol; this repo only supplies the window, WebGPU surface, threads, and
browser-shaped shims.

The engine connects over **WebTransport draft-02** (QUIC/HTTP/3). Everything
runs locally; no wasm patching is required.

## Run

```sh
cd client
WC_COOKIE_FILE=/path/to/cookie.txt ./client/client 189707
```

- Place **189707** is the known-good test place (reports `#players = 21`).
- The cookie must be a valid `.ROBLOSECURITY` value. A stale cookie hangs at
  `[pthread] thread #1 created` with heartbeats still running — that is not a
  code bug. Only one client should be joined at a time.
- If a run stalls at startup, retry the identical command; the engine has an
  intermittent early-init flake.

Defaults and optional overrides:

| Variable | Default | Purpose |
| --- | --- | --- |
| `WC_COOKIE_FILE` | `main.ts` default | path to `.ROBLOSECURITY` |
| `WC_WASM_PAGES` | `16384` | guest memory pages (1 GiB) |
| `WC_PLACE` | positional arg | place to join |
| `WC_NATIVE_JOIN_UA=1` | off | A/B: use native gamejoin identity but keep WebTransport and `gameFqdn` derivation |

## Build

The binary must be built with `--unstable-net` so `Deno.connectQuic` is baked in:

```sh
cd client
deno desktop -A --unstable-net --include page --include mirror main.ts
```

Stop when `Bundle client` appears. The launcher is `client/client`, the
embedded app is `client/client.so`.

## Layout

| path | what it is |
| --- | --- |
| `client/` | **the product** — Deno host, WebGPU, draft-02 WebTransport, shims |
| `docs/` | engine snapshots, import surface, and the [research archive](docs/ARCHIVE.md) |
| `REGENERATING.md` | how to recreate the gitignored engine mirror and captures |
| `client/INVESTIGATION.md` | historical pre-fix record — **outdated**, see the banner |

Generated artifacts (engine mirror, caches, captures, `.har` files) are not in
git. Recreate them with [REGENERATING.md](REGENERATING.md).

Archived host experiments live on local `experiment/*` branches, not in the
working tree. See [docs/ARCHIVE.md](docs/ARCHIVE.md) for what they contain and
why they were retired.

## Two facts worth keeping

- The join identity matters: `rbx-user-agent: Roblox/Darwin` on the gamejoin POST
  with the browser `User-Agent` gives a usable reply and a derived `gameFqdn`.
  The engine's own `...Browser/Chromium/...` value gets **status 12,
  "Playtesters Only"**. The old "session gate" theory was this, not a protocol
  limitation.
- Roblox/qdmux uses **WebTransport draft-02**. Deno's built-in WebTransport
  cannot send `sec-webtransport-http3-draft02: 1`, so
  `client/page/draftwt.ts` implements that header over raw QUIC. Four
  alternative transport libraries failed identically during investigation —
  do not re-add them.
