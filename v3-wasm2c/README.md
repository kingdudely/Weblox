# wasm2c path (parked, being revived)

This directory held the WABT `wasm2c` host experiment. It is **parked**, not
part of the supported runtime. See `docs/ARCHIVE.md` §5 for why it was parked:
the generated C is opaque, regenerates weekly, and measured guest CPU was only
2–10% — so AOT gains were irrelevant at the time. GitHub Actions now removes
the rebuild-cost objection, so this path is under evaluation again.

## What is tracked here

- `*.c` — hand-written host import implementations (`glue.c`, `embind.c`,
  `fetch.c`, `futex.c`, `thread.c`, `rna.c`, `time.c`, `wasi.c`, …).
- `full.sh`, `build_host.sh`, `link.sh`, `check.sh`, `b.sh` — build scripts.
- `mk.py`, `roots.txt` — the (lost) reachability slicer's driver and seed list.
- `tools/` — import/GL/WasmBridge generators.

## What was never committed

The generated tree and the host glue lived only on disk (formerly in the
deleted `~/Alcohol` tree):

- `WC.h`, `WC-impl.h`, `WC_*.c` (generated C; ~2.3 GB for 32 outputs)
- `host/host.h`, `host/plat.h`, `host/gl.h`, `host/*.c`
- `impl/*.c`
- the one-function-per-file splitter used by `mk.py`

`REGENERATING.md` §3 describes the original reproduction steps and calls out
the missing splitter explicitly. Do not rely on it as-is.

## Current build path (CI)

The portable pipeline is in `tools/ci/`:

- `engine.sh` — translate the pinned stock wasm with WABT 1.0.42 and compile
  the generated C. CI publishes per-OS engine bundles plus a standalone
  `WC.h` to the Release tagged with the pinned Roblox version.
- `fetch_engine.sh` — download engine assets from a CI Release on any machine.
- `gen_stubs.py` — generate all host import stubs **from `WC.h`** (no indices).
- `smoke.c` / `smoke.sh` — instantiate the module using those stubs.

`REGENERATING.md` §3's manual steps are superseded by `tools/ci/`.

The portable C++ host in `cpp/` is built with stock CMake >= 3.20 and the
`zstd` CLI; see `cpp/README.md`. At configure time it downloads the pinned
bundle and `WC.h`, verifying SHA-256 values from `tools/ci/pins.env`. The
default engine mode consumes the prebuilt per-OS shared library;
`-DWEBLOX_ENGINE_MODE=source` compiles the pinned generated-C bundle locally
as the portable fallback (slow, validated in CI).

## Known constraints

- WABT 1.0.36 cannot parse the current module's `exnref` encoding.
- WABT 1.0.42 translation of the current pinned build aborts inside
  `WriteC` at ~4.9 GiB peak. The older stock build
  `version-f807b21caba84809` (169,929 functions, all 12 RNA imports) was
  translated successfully on this machine with 32 outputs.
- `smoke.c` only proves instantiation; there is no join-capable wasm2c host
  until the missing `host/` and `impl/` sources are reconstructed.

## Verified 2026-11-27

- CI compiled the 32-TU generated tree with clang `-O0` in 590 s into a
  1.11 GB `libweblox_engine.a` (run `45934107270`, Release `ci-45934107270`).
- `tools/ci/fetch-and-smoke.sh` downloaded it, generated 277 import stubs from
  `WC.h`, linked a 949 MB executable and instantiated the module successfully:
  `SMOKE_OK mem=268435456`, rc=0 (run `45935598599`).
- This proves the plain-C ABI path works end-to-end without any guest indices
  or manual pointers. A join-capable host is still future work.
