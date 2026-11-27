# Native engine build (CI)

GitHub Actions is a **builder only**. It does not resolve versions, match
Releases, or orchestrate host work. An AI (or a human) reads this directory,
edits the C sources in `v3-wasm2c/`, and dispatches a build.

## Inputs

`pins.env` is the source of truth for the engine build:

```
WASM_VERSION=1ef59424f70d4932
WASM_SHA256=be370a44...
```

Update those two lines when Roblox ships a new build. Environment variables
override them for one-off experiments.

## Canonical scripts

| Script | Does |
|---|---|
| `common.sh` | shared helpers: fetch/hash pinned wasm, fetch WABT, metrics |
| `engine.sh` | translate wasm with WABT wasm2c, compile generated C, archive `libweblox_engine.a` + `WC.h` |
| `compile-generated.sh` | compile an existing generated C tree (fallback when translation is broken) |
| `fetch_engine.sh` | download built assets from the newest `ci-*` Release |
| `gen_stubs.py` | generate every host import stub from a generated `WC.h` — no indices, no guest pointers |
| `smoke.c`, `smoke.sh` | minimal example host: instantiate the module, print `SMOKE_OK` |
| `metrics.py` | fold metrics + `time -v` into JSON |

## Build

```sh
gh workflow run heavy.yml -f task=wabt-wasm2c -f opt=O0 -f outputs=32
gh run watch <run-id> --exit-status
```

Other useful dispatches:

```sh
# compile the pre-generated 2.3 GB C tree instead of translating
gh workflow run heavy.yml -f task=custom \
  -f script=tools/ci/compile-generated.sh -f opt=O0 -f timeout-minutes=360

# download the built library and run the smoke example
gh workflow run heavy.yml -f task=custom \
  -f script=tools/ci/fetch-and-smoke.sh -f upload-release=false
```

The workflow publishes `libweblox_engine.a`, `WC.h`, `WC-impl.h`, `manifest.json`
and `env.sh` to a Release. Small logs/metrics go to an Actions artifact.

## Host development

The host is plain C source in `v3-wasm2c/` plus new files an AI writes there.
`WC.h` is the ABI contract: include it, implement the import functions (or
generate stubs with `gen_stubs.py`), link against the library. Do **not**
hardcode guest indices, function numbers, or linear-memory addresses — derive
them from the generated header/data instead.

`v3-wasm2c/README.md` records what was never committed and what is still
missing. `docs/ARCHIVE.md` explains why this path was parked.

## Diagnostics (not canonical)

These exist to record failed experiments; do not build product logic on them:

- `wabt-probe.sh`, `wabt-version-probe.sh`, `wabt-source-probe.sh`
- `w2c2.sh` (no EH support)
- `cranelift-object.sh`
- `fetch-and-smoke.sh` (convenience wrapper)
- `host.sh` (generic host link helper)
