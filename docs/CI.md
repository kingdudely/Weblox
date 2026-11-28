# CI

Two GitHub Actions workflows cover the clean C host. Both use only stock
CMake (3.20+), Python 3, `zstd` and a C11 compiler.

## `host-smoke.yml`

Portability gate for the host. Runs on `workflow_dispatch`, `workflow_call`,
and pushes to `host-c` that touch CMake sources, `cmake/`, `deps/`, `src/`,
`tools/` or the workflow itself. Permissions are read-only and runs for the
same ref are serialized without cancelling a running matrix: new pushes queue
instead of killing the current run.

A Linux/macOS/Windows matrix (`fail-fast: false`, `bash` shell) verifies the
toolchain (installing `zstd` only when absent, checking CMake >= 3.20), then
runs configure, build and `ctest` as separate steps. No distro packages are
installed for libraries: `cmake/Dependencies.cmake` builds pinned curl and SDL3
source tarballs when system packages are unavailable. Each step tees its output into
`ci-logs/`; the logs are uploaded as an artifact on failure. Windows configures
with `-DWEBLOX_ENGINE_MODE=source`, because the Windows DLL has no `wasm_rt_*`
exports; Linux and macOS consume the prebuilt engine bundle. Source mode
compiles the generated C at `WEBLOX_ENGINE_OPT=O0` (default) so the validation
build stays fast; shipped bundles are built at O3 by `build-engine.yml`. A
passing test prints `SMOKE_OK`.

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
and `WABT_SOURCE_SHA256`, `CURL_SHA256`, `SDL3_SHA256`, `YYJSON_SHA256` and
`MINICORO_SHA256`, plus the new `DAWN_VERSION`/`DAWN_SHA256`,
`NGTCP2_VERSION`/`NGTCP2_SHA256`, `NGHTTP3_VERSION`/`NGHTTP3_SHA256` and
`SFPARSE_VERSION`/`SFPARSE_SHA256` (nghttp3's git submodule).
`cmake/FetchAsset.cmake` passes each hash to
`file(DOWNLOAD ... EXPECTED_HASH SHA256=...)`, so a tampered or stale asset is
rejected before it is unpacked. `build-engine.yml` applies the same rule to
the generated-C bundle and SIMDE tarball with an explicit SHA-256 check before
use. Nothing is trusted based on the URL alone.

## Optional native backends

`cmake/Dependencies.cmake` fetches the pinned ngtcp2 and nghttp3 tarballs with
the curl + `file(SHA256)` + `FetchContent SOURCE_DIR` pattern used for curl and
SDL3. It configures each once into `${build}/assets/<name>-build` with
`ENABLE_LIB_ONLY=ON`, `ENABLE_STATIC_LIB=ON`, `ENABLE_SHARED_LIB=OFF` and
`BUILD_TESTING=OFF`, then links the resulting archives once into `weblox`.
`WEBLOX_HAVE_NGTCP2`, `WEBLOX_HAVE_NGHTTP3` and `WEBLOX_HAVE_NGTCP2_TLS` are
defined only when the corresponding archive is actually linked; a failed
download, failed nested build or missing TLS provider leaves the original
no-op logging path and the smoke test intact. No distro packages are needed:
OpenSSL, when present, supplies QUIC TLS via `libngtcp2_crypto_ossl`.

Dawn is pinned at commit `2dc7e645fb7e7d8d10f37bd0e02f622c662e8177`
(`DAWN_VERSION`); its hash-verified tarball can be fetched with
`-DWEBLOX_DAWN_SOURCE=ON` for the `include/` tree. Dawn's GN build needs
depot_tools plus third_party checkouts that GitHub's tarball does not contain,
so a self-contained Dawn library build is **not** attempted here. A real
WebGPU backend is enabled only when a linkable `webgpu_dawn` library is found
(`-DWEBLOX_DAWN_LIBRARY=/path/to/libwebgpu_dawn.so`) together with its
`webgpu/webgpu.h`; that defines `WEBLOX_HAVE_DAWN`. Native-window surfaces are
compiled when a CMake compile probe finds the platform descriptor in the
installed header (on by default, `-DWEBLOX_DAWN_SURFACE=OFF` to disable).

`webgpu.c` implements a real Dawn instance, adapter and device with the WebGPU
C API and forwards known names from `webgpu_dispatch()` to real Dawn functions.
`webtransport.c` implements QUIC connect, TLS/ALPN `h3`, certificate pin
verification from `joinScript`, an HTTP/3 CONNECT stream, stream DATA in both
directions, DATAGRAM send/receive and close, all guarded by SDL3 (timers) and
guarded platform socket headers.

Remaining `WT_TODO` items: the HTTP/3 CONNECT response status and WebTransport
draft headers are not validated (any completed header block accepts the
session); unsolicited engine stream IDs are tracked but not mapped to new QUIC
bidi streams; partial DATAGRAM acks are reported as immediate acks. None of
these affect transport setup or the smoke test.

GLES dispatch resolves every imported `gl*` symbol through
`SDL_GL_GetProcAddress` (or GLAD when built), queries real context attributes,
and returns invalid GL values rather than fabricated capabilities when an entry
point is missing.
