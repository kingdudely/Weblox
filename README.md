# Weblox C host

A clean, portable C11 host for the wasm2c-generated Roblox WebClient engine.
No browser, no C++, no Rust, no generated-source edits.

## Build

Requires CMake 3.20+, `zstd`, Python 3 and a C11 compiler.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake downloads the pinned engine bundle and `WC.h` from the Release named in
`deps/pins.env`, verifies SHA-256, and generates import stubs from `WC.h`.
`WEBLOX_ENGINE_MODE=source` compiles the pinned generated-C bundle locally
instead of consuming the prebuilt shared library.

## Layout

- `src/` — host C sources; imports are named exactly as in generated `WC.h`.
- `deps/wabt/` — WABT 1.0.42 runtime headers, pinned for ABI compatibility.
- `deps/pins.env` — every download hash.
- `cmake/FetchAsset.cmake` — hash-verified download/unpack.
- `tools/gen_stubs.py` — generates no-op imports from `WC.h`.
- `tools/make_overlay.py` — build-directory VFS overlay that replaces WABT's
  no-op atomic wait/notify stubs. The published generated C is never edited.
  This applies to source-mode builds; prebuilt engine bundles must be rebuilt
  with the same overlay by CI (`build-engine.yml`) before their atomics work.
- `src/http.c`, `src/fetch.c`, `src/fs.c` — libcurl HTTP and asset fetch imports,
  using yyjson for join-reply patching and a queued completion pump.
- `src/window_sdl.c`, `src/gl.c`, `src/webgpu.c`, `src/webtransport.c` — SDL3
  window/input, GL ES, Dawn and ngtcp2/nghttp3 layers behind C interfaces.

HTTP, SDL3, Dawn and QUIC support are optional at build time: when a library
is unavailable, those imports stay as generated no-ops.
