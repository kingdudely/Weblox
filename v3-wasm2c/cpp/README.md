# C++ host skeleton

This is the start of the native host. The generated engine library is
**plain C** (`WC.h` is a C header), so the host includes it inside
`extern "C"` and links against objects compiled as C.

```
host.cpp       instantiate the module, print CPP_SMOKE_OK, run a short loop
cpp_compat.h   include before WC.h in every C++ TU (C11 struct layout)
guest.h        bounds-checked guest memory access (no hardcoded addresses)
mem.h/.c       C shim for guest memory (C11 _Atomic is not usable from C++)
platform.h     std::chrono / std::thread / std::condition_variable services
imports.cpp    core imports (console/clock/random/WASI)
host_info.cpp  host identity, canvas, DPR and device queries
asm_const.cpp  EM_ASM closure dispatch (page-side keys)
thread_init.cpp main-thread TLS/thread setup
loop.cpp       event-loop inversion with wasm exception guards
fiber_impl.cpp, thread_host.cpp, futex_host.cpp, mailbox.cpp, deferred.cpp
               JSPI fibers, pthreads, futexes and the deferred queue
fetch_curl.cpp, http_fetch.cpp, fs_cache.cpp
               libcurl HTTP/fetch and the local content cache (optional)
implemented-cpp.txt  imports this host implements; all others get stubs
window.h       window/input abstraction; window_sdl.cpp is the SDL3 opt-in
net.h          HTTP abstraction; net_curl.cpp is the curl opt-in
gpu.h          WebGL/WebGPU abstraction; gpu_stub.cpp is the no-op default
fiber.h        stackful fibers; fiber_ucontext.cpp (POSIX) / fiber_win.cpp (Windows) are opt-in
CMakeLists.txt stock CMake build (prebuilt default, source fallback)
abi/           WABT 1.0.42 runtime headers and sources (exact engine ABI)
```

## Rules

1. Never include `WC.h` without `extern "C"` from C++.
2. Never compile the generated `WC_*.c` as C++ — compile as C, link as C.
3. No hardcoded guest indices, pointers or linear-memory addresses in host
   code. Import stubs come from `gen_stubs.py` reading the generated `WC.h`;
   `implemented-cpp.txt` lists the only names the host defines itself.
4. Platform-specific code goes behind `window.h` (and future `gpu.h`,
   `net.h`, `fiber.h`) abstractions, not `#ifdef` in `host.cpp`.

## Planned APIs

| Need | API |
|---|---|
| Window/input/audio | SDL3 (`window_sdl.cpp`) |
| WebGL | SDL_GL + GLAD, or ANGLE if semantics diverge |
| WebGPU | wgpu-native behind `gpu.h` |
| HTTP | libcurl |
| WebTransport | quiche |
| JSON | nlohmann/json |
| Fibers | POSIX ucontext / Win32 fibers behind `fiber.h` |

## Build

Prerequisites: stock CMake >= 3.20 and the `zstd` CLI. No CMake upgrade is
needed. libcurl is optional: when found, the HTTP/fetch imports are real; when
absent they remain generated no-ops and the join path is unavailable.

```sh
cmake -S v3-wasm2c/cpp -B build/cpp
cmake --build build/cpp
ctest --test-dir build/cpp --output-on-failure
```

Success prints `CPP_SMOKE_OK`.

### Engine modes

| `-DWEBLOX_ENGINE_MODE=` | Behaviour |
|---|---|
| `prebuilt` (default) | Consume the pinned per-OS engine shared library. |
| `source` | Compile the pinned generated-C source bundle locally. Portable fallback for platforms with no prebuilt binary; this is the slow path and is validated in CI. |

At configure time CMake downloads the pinned engine bundle and the standalone
`WC.h` Release asset. SHA-256 values come from `tools/ci/pins.env`; no hash is
trusted from the URL.

`abi/wasm-rt.h` and `abi/wasm-rt-exceptions.h` are vendored from WABT 1.0.42 so
the host compiles against the same runtime structs as the prebuilt engine. The
host also compiles the WABT runtime sources, so it does not depend on
`wasm_rt_*` symbols being exported by the Windows DLL.

Import stubs come from `WC.h` via `tools/ci/gen_stubs.py`; there are no
hardcoded import indices. Platform code stays behind `window.h`, `net.h`,
`gpu.h` and `fiber.h`: the default build uses stubs, while curl, SDL3,
ucontext and Win32 implementations are opt-in for later phases.

`WC.h` is a build-time input only; end users never see it. The end-user
artifact (planned, not built yet) is one archive per OS containing the host
executable, the engine shared library and the runtime libraries, found
relative to the executable.

This skeleton proves the C++ ↔ generated-C boundary works. A join-capable
host requires filling in the abstractions above.
