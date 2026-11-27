# CI

Two GitHub Actions workflows cover the clean C host. Both use only stock
CMake (3.20+), Python 3, `zstd` and a C11 compiler.

## `host-smoke.yml`

Portability gate for the host. Runs on `workflow_dispatch`, `workflow_call`,
and pushes to `host-c` that touch CMake sources, `cmake/`, `deps/`, `src/`,
`tools/` or the workflow itself. Permissions are read-only and runs for the
same ref are serialized with cancellation of older runs.

A Linux/macOS/Windows matrix (`fail-fast: false`, `bash` shell) verifies the
toolchain (installing `zstd` when absent, checking CMake >= 3.20), then runs
configure, build and `ctest` as separate steps. Each step tees its output into
`ci-logs/`; the logs are uploaded as an artifact on failure. Windows configures
with `-DWEBLOX_ENGINE_MODE=source`, because the Windows DLL has no `wasm_rt_*`
exports; Linux and macOS consume the prebuilt engine bundle. A passing test
prints `SMOKE_OK`.

## `build-engine.yml`

Manual (`workflow_dispatch`) builder for the prebuilt engine bundles. Inputs:

- `opt` — optimisation level for the generated C (default `O3`).
- `force` — overwrite assets that already exist on the Release (default false).

The same three-platform matrix fetches the pinned `generated-c.tar.zst`
(falling back to the `ci-local-wasm2c-generated` Release), compiles the
generated `WC_*.c` together with the vendored WABT runtime and pinned SIMDE
headers into one shared library, then packs the library and `WC.h` into
`engine-<platform>.tar.zst`. The bundle is uploaded to the Release named by
`WASM_VERSION` with three retries and an idempotent skip when the asset
already exists (unless `force` is set). A concurrency group prevents two runs
from racing on the Release asset endpoints.

If `force` is used, the new archive bytes change: update the corresponding
`ENGINE_*_SHA256` value in `deps/pins.env` in the same commit, otherwise host
downloads will fail hash verification.

## Hash pinning

`deps/pins.env` is the single source of truth for every download. CMake reads
`WASM_VERSION`, `ENGINE_LINUX_SHA256`, `ENGINE_MACOS_SHA256`,
`ENGINE_WINDOWS_SHA256`, `WC_H_SHA256`, `GENERATED_C_SHA256`, `SIMDE_SHA256`
and `WABT_SOURCE_SHA256`. `cmake/FetchAsset.cmake` passes each hash to
`file(DOWNLOAD ... EXPECTED_HASH SHA256=...)`, so a tampered or stale asset is
rejected before it is unpacked. `build-engine.yml` applies the same rule to
the generated-C bundle and SIMDE tarball with an explicit SHA-256 check before
use. Nothing is trusted based on the URL alone.
