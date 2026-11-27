# Two-stage native build strategy (2026-11-26)

## Problem

The translated engine is enormous: 154,550 functions, a 92 MB code section and
~688 MiB of generated C. Rebuilding it on a developer machine is impossible
(the box has ~2 GB free RAM) and it is the same work every time a host import
is edited.

## Solution

Split the build into two independent stages.

```
Stage A (rare, ~minutes)                 Stage B (every edit, seconds)
────────────────────────────             ─────────────────────────────
WebClient.wasm (pinned, hashed)          host/*.c, imports, GPU, RNA
        │                                          │
        │ wasm2c --num-outputs=512                  │
        ▼                                          │
WC*.c + WC.h                                       │
        │                                          │
        │ cc -O1 -j$(nproc)                         │
        ▼                                          │
libweblox_engine.a ─────────────────────────────────┘
        + WC.h                    linker
        + manifest.json              │
        + env.sh                     ▼
                              weblox executable
```

Stage A runs **only** when the wasm, toolchain, flags or translation script
change. Its output is keyed by a hash of all of those, cached locally under
`build/ci/engine/<key>/`, and published to a GitHub Release so any machine can
download it. Stage B never recompiles the engine.

## Scripts

| Script | Job |
|---|---|
| `tools/ci/common.sh` | shared helpers: pinned wasm download + sha256, WABT fetch, feature-flag probing, timing, metrics |
| `tools/ci/engine.sh` | Stage A: translate → compile → `libweblox_engine.a` + `WC.h` + `manifest.json` + `env.sh` |
| `tools/ci/host.sh` | Stage B: compile host sources and link against the cached `.a`; header-only check when no host sources exist |
| `tools/ci/metrics.py` | fold `metrics.txt` + GNU `time -v` into JSON |
| `.github/workflows/heavy.yml` | `workflow_dispatch` / `workflow_call`, publishes Release assets and uploads small logs |

Every path has an env override (`WC_WASM`, `WASM2C`, `WABT_ROOT`, `SIMDE_DIR`,
`CC`, `WC_OPT`, `WC_OUTPUTS`, `WC_JOBS`, `WC_BUILD_DIR`, `WC_ENGINE_DIR`,
`WC_HOST_SRCS`). Nothing is hardcoded to `/home/john`.

## Run it

### Locally (only for light steps)

```sh
# Stage A: normally skip; use the Release asset instead
tools/ci/engine.sh

# Stage B: seconds
WC_HOST_SRCS="host/main.c host/imports.c" tools/ci/host.sh
```

### GitHub Actions (all heavy work)

```sh
gh workflow run heavy.yml \
  -f task=wabt-wasm2c -f opt=O1 -f outputs=512
gh run watch

# a subagent's own branch/script
gh workflow run heavy.yml --ref rust-wt \
  -f task=custom -f script=tools/ci/w2c2.sh -f opt=O0
gh run watch
```

`workflow_call` is enabled too, so another workflow or agent can dispatch the
same job.

## Where the output goes

| Data | Destination | Why |
|---|---|---|
| `libweblox_engine.a`, `WC.h`, `manifest.json`, `env.sh` | **GitHub Release** `ci-<run_id>` | 2 GiB/file, no total size limit, anonymous download, not billed |
| `metrics.json`, logs, `time.txt` | Actions artifact (`heavy-<task>-<opt>-<run_id>`) | small, expire automatically |
| toolchains / downloaded wasm | `build/ci/` (gitignored) | local cache only |

Actions artifacts are **not** used for binaries: 500 MB limit and they expire.
Release assets are a convenience cache, not a build dependency — the same
scripts work on any machine with a C compiler and `curl`.

## Portability

- WABT prebuilt tarballs are selected from `uname -s`/`uname -m`;
  `WABT_ROOT`/`WASM2C` override them for system installs.
- Feature flags are discovered from `wasm2c --help`, so WABT releases that
  rename `--enable-X` to `--disable-X` still work.
- `WC.h` is the generated ABI contract. When the engine updates, regenerate
  the library; the host only needs edits if the import/export surface changed.
- `WC-impl.h` is created empty when absent (it is a splitter artifact of the
  parked `v3-wasm2c` tree, not a stock WABT output).

## Rules

1. Anything that translates, compiles, or runs for minutes goes to Actions.
2. Never patch the wasm; generate indices/offsets from the module at Stage A.
3. Never hardcode guest pointers or import indices in the host.
4. Compare tools by the numbers in `docs/CI-RESULTS.md`, not by intuition.
