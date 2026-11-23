# Regenerating generated artifacts

This repository intentionally **does not ship** large generated/build artifacts.
They are either downloaded from Roblox's CDN on first run, or produced from the
cached engine wasm by a local tool. This file tells you exactly how to recreate
them. Nothing in here requires network access to this machine's home directory
paths except where noted.

If you are an AI reading this: **do not try to "restore" these files from git.**
They were removed on purpose. Generate them with the steps below.

---

## 1. `client/mirror/` — cached engine + page assets (gitignored)

The Deno client needs Roblox's own `WebClient.js`, `WebClient.wasm`, and asset
files. These are fetched from `setup.rbxcdn.com` automatically on first run and
cached in `client/mirror/files/`.

```sh
cd client
./run.sh 189707
```

On a cold start, `page/net.ts` resolves the current engine version from
`https://setup.rbxcdn.com/webclient/` and downloads everything referenced by
`WebClient.js` / `WasmPlayer.js`. The resolved version is pinned in
`client/mirror/engine.json`, so subsequent runs are offline.

- To force a refresh against a new Roblox build: `rm -rf client/mirror` (or set
  `WC_REFRESH=1`).
- To pin a specific build: set `WC_ENGINE` to the full base URL, e.g.
  `https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient`.
- The current known-good build is `version-1ef59424f70d4932-RobloxWebClient`.
  It must match what Chrome loads (verify against the HAR in the investigation).

There is a **reference copy** of the engine files under
`docs/reference/research/` (`WebClient.js`, `WebClient.wasm`, `WasmPlayer.js`,
`WasmLoader.js`) that *is* tracked, but it is for study only — the client reads
from `client/mirror/`.

---

## 2. `v2-wasmtime/data/cache/` and `v2-wasmtime/data/content/` (gitignored)

The Rust/wasmtime harness generates both of these at runtime:

- `data/cache/webclient.cwasm` — wasmtime's compiled artifact cache. On the
  first `cargo run`, `main.rs` compiles `WebClient.wasm` and writes the cached
  `.cwasm` plus a `.stamp`. Later runs are instant.
- `data/content/` — the engine's asset tree. `src/imports/fetch.rs` fetches each
  requested file from the CDN and caches it back here.

To regenerate, just build and run once:

```sh
cd v2-wasmtime
cargo build --release
cargo run --release -- 189707
```

The input wasm defaults to the engine copy under `data/content`; set `WC_CONTENT`
to point at a different asset tree, and `WC_WASM` to point at a different wasm,
if needed. To force recompilation, delete `data/cache/webclient.{cwasm,stamp}`.

`data/meta.json` and `data/opfs/` are small and **are** tracked — leave them.

---

## 3. `v3-wasm2c/gen/` — wasm2c translation (gitignored)

This is the big one: a 512-way split of the wasm2c translation of
`WebClient.wasm`, produced by **wabt** and a local slicer.

Prerequisites:
- `wasm2c` from wabt (a system `wasm2c` is on `PATH`; the build scripts also
  reference `/home/john/deps/wabt/wasm2c` for the runtime headers).
- `mk.py` (in `v3-wasm2c/`) — splits the monolithic translation by function and
  builds `gen/sliced.c` from a reachability seed list in `roots.txt`.

Steps (paths are relative to `v3-wasm2c/`):

```sh
cd v3-wasm2c

# a) translate the wasm into C (produces WC.c / WC.h)
wasm2c /path/to/WebClient.wasm -o WC.c        # emits WC.h alongside

# b) split WC.c into one function per file (WC_0.c ... WC_511.c) and
#    assemble the reachability-ordered sliced.c. The current tree used a
#    one-function-per-file split; mk.py then reads those files plus roots.txt.
#    (The exact splitter used originally is not checked in; any splitter that
#     names functions w2c_WebClient_fNNNN and leaves globals/tables in WC_0.c
#     works with mk.py.)
python3 mk.py

# c) compile the whole thing
sh full.sh
```

`full.sh` compiles the 512 translation units in parallel (via `xargs -P
$(nproc)`), reuses unchanged objects via a stamp, then links `luau_probe2`.
`build_host.sh` / `link.sh` are the fast host-only relink paths. It expects the
runtime headers from wabt at `/home/john/deps/wabt/wasm2c` and SIMDE at
`/home/john/deps/simde`, and uses a local ccache wrapper if present
(`WC_NO_CCACHE=1` to disable).

Because the original one-function-per-file splitter is not checked in, the most
reliable way to reproduce the exact `gen/` tree is to regenerate it from the
same wasm build (`version-1ef59424f70d4932`) with the same wabt version. If you
only need to *run* the harness, the Rust `v2` harness is the maintained path;
`v3` is a parked experiment.

---

## Summary of what is tracked vs generated

| Path | Tracked? | Recreate with |
|---|---|---|
| `client/mirror/**` | no | `cd client && ./run.sh` (auto-download) |
| `v2-wasmtime/data/cache/**` | no | `cargo run` in `v2-wasmtime` |
| `v2-wasmtime/data/content/**` | no | `cargo run` (CDN fetch) |
| `v3-wasm2c/gen/**` | no | `wasm2c` + `mk.py` (see above) |
| `*.pcap`, `*.har`, `*.log` | no | re-capture / re-export |
| All source, scripts, docs, `data/meta.json`, `data/opfs/` | yes | — |
