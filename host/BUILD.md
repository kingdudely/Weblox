# Building the native C++ AOT host

`host/demo` links the Cranelift-emitted object for Roblox web client build
`version-1ef59424f70d4932` into an ordinary C++ executable. There is **no
Wasmtime `Engine`/`Store`/JIT at runtime**, and **no browser, Chrome, Vulkan,
Deno, CEF or game client is involved**. The host supplies the Wasmtime
VMContext ABI itself (see `host/README.md`, `docs/NATIVE-AOT-FINDINGS.md`).

This is an **experimental ABI/integration host**: ctors and imports run, but a
full join is still blocked by native exception propagation (`throw_ref`, see
`host/GC.md` and `host/PREPARE.md`). The build, however, is fully reproducible.

## Prerequisites

| Tool | Why |
| --- | --- |
| `python3` | `patch_new.py`, `gen_table.py`, `gen_runtime_data.py`, gate patch |
| `make`, `clang++` (or `g++`) | final link |
| `cargo` + a Rust toolchain | `tools/wasm2obj` and `host/build_quiche.sh` |
| `curl` | optional: fetch the stock engine module; also used by `route.cpp` |
| ~8 GB RAM, ~30 GB free disk | 342 MB object, 360 MB executable, cargo build trees |

The cargo registry cache must contain quiche 0.30.1 (+ BoringSSL) and the
vendored Wasmtime crates; `build_quiche.sh` and `tools/wasm2obj` try offline
first and fall back to the network.

### Where to get the stock wasm

Download the official web client module for the pinned build and check its
hash:

```sh
curl -L --fail -o WebClient.wasm \
  https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient/WebClient.wasm
sha256sum WebClient.wasm
# be370a4446dcfce62635f7e6d271526b15e2099507fb7250b2d3360eacd4a61f  102268042 bytes
```

On this machine the same file is at `/home/john/rr/WebClient.wasm` and the
local mirror `/tmp/opencode/WebClient.orig.wasm`. Engine builds move weekly:
`patch_new.py` asserts the exact stub bodies/signatures and aborts on a
different build rather than patching the wrong bytes (you would then need to
re-resolve its indices; see `v2-wasmtime/tools/README.patch_new.md`).

## One-shot build

```sh
cd /path/to/Weblox
./host/build_all.sh /path/to/WebClient.wasm
cd host && timeout 60s ./demo --skip-ctors
```

`build_all.sh` runs five stages, writes nothing into the repo, and prints each
artifact path/size:

1. **Gate patch** — `func[17777]`'s 36-byte body (file offset `0x117c06c` in
   the stock module) is replaced with `i32.const 1; nop x32; end` so the
   browser-side play gate always passes. It is done through `patch_new.py`'s
   own section parser (function index + exact body assertion), not a blind
   byte edit. Output: `WebClient.gated.wasm`, 102,268,042 B.
2. **`patch_new.py --all`** — socket/RakNet ring patches. Output:
   `WebClient.raknet.wasm`, 102,267,785 B, sha256
   `2130db0da4d6e5d512e5d2c70ea476f1bd5d5dd2b047feb0260d0abceb7b5299`,
   plus `WebClient.raknet.abi.json` for the (unused here) page bridge.
3. **`wasm2obj --all`** — 154,552 functions compiled with rayon. Output
   `out.full.o`, 342,188,600 B. Older notes quote sha256
   `fa5f371d82285703…` for the object archived in `docs/NATIVE-AOT-FINDINGS.md`;
   the current tool build emits the same size but a different hash (tool
   settings changed since). Functional equivalence was verified by linking the
   freshly built object and rerunning the smoke test.
4. **`build_quiche.sh`** — static libquiche 0.30.1 + BoringSSL in
   `$WC_QUICHE_DIR`: `include/quiche.h` (53,777 B),
   `lib/libweblox_quiche.a` (31,126,056 B), `link.mk`.
5. **Generators + link** — `gen_table.py`, `gen_runtime_data.py`, then `make
   demo`. Output `host/demo` (~360 MB).

Everything is overridable from the environment; the defaults are:

| Variable | Default | Meaning |
| --- | --- | --- |
| `WC_STOCK_WASM` | `/tmp/opencode/WebClient.orig.wasm` | input to stage 1 (or pass `$1`) |
| `WC_WASM` | `/tmp/opencode/WebClient.raknet.wasm` | patched module (stages 2–5) |
| `WC_OBJ` | `/tmp/opencode/out.full.o` | Cranelift object (link input) |
| `WC_QUICHE_DIR` | `/tmp/weblox-quiche` | libquiche prefix; `Makefile` falls back to `/tmp/wl-qtry-prefix` if it has a `link.mk` |
| `WASM2OBJ` | `<repo>/tools/wasm2obj/target/release/wasm2obj` | compiler binary |
| `JOBS` | `4` | `make -j` parallelism |
| `WC_SKIP_PATCH` / `WC_SKIP_OBJ` / `WC_SKIP_QUICHE` | `0` | set to `1` to reuse existing artifacts |
| `QUICHE_VERSION`, `QUICHE_CRATE_DIR` | `0.30.1`, cargo registry | `build_quiche.sh` inputs |
| `WASM`, `WASM_OBJ`, `QUICHE_PREFIX` | aliases | older spellings still accepted by `Makefile` |

`QUICHE_PREFIX` and `WC_QUICHE_DIR` are aliases: `build_all.sh` sets both when
calling `build_quiche.sh`. Generated `table.cpp`, `runtime_data.cpp` and
`runtime_data.h` are regenerated from `WC_WASM`; only the headers `table.h`
and `runtime_data.h` are tracked in git.

## Stage-by-stage commands (same as the script)

```sh
# 1+2: patched wasm (a few seconds for the 100 MB parse/patch)
./host/build_all.sh /path/to/WebClient.wasm        # or set WC_SKIP_* below

# 2, standalone:
python3 v2-wasmtime/tools/patch_new.py WebClient.gated.wasm \
  /tmp/opencode/WebClient.raknet.wasm --all \
  --abi /tmp/opencode/WebClient.raknet.abi.json

# 3: build the tool once (~3 m 50 s cold), then compile (~260 s on 8 cores)
cargo build --release --manifest-path tools/wasm2obj/Cargo.toml
tools/wasm2obj/target/release/wasm2obj --all \
  --out /tmp/opencode/out.full.o /tmp/opencode/WebClient.raknet.wasm
# or: make -C host obj WC_WASM=... WC_OBJ=...

# 4: first build ~6 min (BoringSSL), then incremental
WC_QUICHE_DIR=/tmp/weblox-quiche ./host/build_quiche.sh

# 5: generated sources (~5 s) + link (~20 s)
cd host
python3 gen_table.py      --wasm "$WC_WASM" --out-dir .
python3 gen_runtime_data.py --wasm "$WC_WASM" --out-dir .
make -j4 demo             # uses WC_WASM / WC_OBJ / WC_QUICHE_DIR
```

Expected generator output:

```
table: declared=161491 initialized=161490 imports=5 defined=161485 distinct_defined=94438 first_defined_func=341
generated 3689 data segments (8782885 bytes)
```

Generated sizes: `table.cpp` 16,527,014 B, `runtime_data.cpp` 28,324,807 B.

### Measured on this image (2026-11-26, 8 cores, `g++`)

| Stage | Time |
| --- | --- |
| stock -> patched wasm (gate + `patch_new.py --all`) | ~5 s (verified sha above, byte-identical) |
| `cargo build --release` for wasm2obj, cold | **3 m 52 s** |
| `wasm2obj --all` (154,552 funcs) | **263.4 s** codegen + 7.4 s object emit = 274 s |
| `build_quiche.sh`, cold (from `host/WT.md`) | ~6 min; incremental afterwards |
| `gen_table.py` + `gen_runtime_data.py` | a few seconds |
| `make -j4 demo` | **1 m 34 s** (g++; `demo` 375,005,424 B) |
| `build_all.sh` with `WC_SKIP_OBJ=1 WC_SKIP_QUICHE=1` | **1 m 17 s** total |

The earlier `clang++` link produced a 360,164,096 B `demo`; `g++` links a
slightly larger binary. Both run the smoke tests.

For an incremental relink after editing C++ only:

```sh
WC_SKIP_PATCH=1 WC_SKIP_OBJ=1 WC_SKIP_QUICHE=1 ./host/build_all.sh
```

## Running

```sh
cd host
timeout 60s ./demo --skip-ctors   # safe ABI smoke test
timeout 60s ./demo --test-trap    # SIGILL -> controlled error
timeout 60s ./demo                # full __wasm_call_ctors
```

For the optional RNA/route path (needs a valid Roblox cookie; routes expire in
minutes):

```sh
WC_PLACE=189707 WC_COOKIE_FILE=/path/to/cookie.txt timeout 90s ./demo --rna-probe
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/path/to/cookie.txt \
  WC_LOOP_MS=3000 timeout 90s ./demo
```

Other runtime knobs: `WC_META` (default `../v2-wasmtime/data/meta.json`),
`WC_CONTENT` (default `data/content`), `WC_SELFTEST_SECS`. See
`host/PREPARE.md` and `host/RNA.md`. **Do not run a join loop unless you own
the account/session and intend to test it**; no game client is launched by this
build.

## Troubleshooting

* **"Vulkan not needed"** — the `--enable-unsafe-webgpu ... --use-vulkan=native`
  flags in `docs/WEB-PLAY-GATE.md` are only for the Chrome IWA path. The native
  host uses quiche/UDP directly; no GPU adapter, Chrome profile or Deno is
  required.
* **`/usr/bin/ld: error in ...out.full.o(.eh_frame); no .eh_frame_hdr table
  will be created`** — expected and non-fatal (exit 0). Cranelift emits
  `.eh_frame` but no `.eh_frame_hdr`; no smoke test needs unwinding.
* **`ASSERT FAILED` from `patch_new.py`** — you fed a different engine build;
  download the pinned `version-1ef59424f70d4932` module or re-resolve indices.
* **Gate-patch assertion about `func[17777]` body** — same cause: wrong stock
  build. The check prints the actual body.
* **`Makefile: /tmp/opencode/out.full.o missing`** — run `make obj` /
  `build_all.sh`, or point `WC_OBJ` at an existing object. Codegen of all
  154k functions takes ~260 s; do not use `--max-funcs` object for execution.
* **quiche source not found in cargo registry** — set
  `QUICHE_CRATE_DIR=/path/to/quiche-0.30.1`, or run `cargo fetch` in
  `tools/quiche-probe` once.
* **`make: *** No rule to make target 'table.cpp'`** — set `WC_WASM` to a valid
  patched wasm; both generated sources depend on it.
* **`throw_ref` / signal 4 / `exception propagation not implemented`** — known
  limitation, not a build failure; see `host/GC.md` and `host/PREPARE.md`.
* **"browser did not provide a WebGPU adapter"**, cookie/HttpOnly, IWA signing
  — those are Chrome/IWA concerns (`docs/IWA.md`), not this host.
* The host `mmap`s a 4 GiB guarded region; a 64-bit OS with memory overcommit
  is required. It reserves address space but commits only what it touches.
