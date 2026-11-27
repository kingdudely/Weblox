# C++ host skeleton

This is the start of the native host. The generated engine library is
**plain C** (`WC.h` is a C header), so the host includes it inside
`extern "C"` and links against objects compiled as C.

```
host.cpp       instantiate the module, print CPP_SMOKE_OK, run a short loop
mem.h/.c       C shim for guest memory (C11 _Atomic is not usable from C++)
platform.h     std::chrono / std::thread / std::condition_variable services
imports.cpp    real C++ implementations of core imports (console/clock/random)
net.h          HTTP abstraction; net_curl.cpp with -DWEBLOX_USE_CURL
gpu.h          WebGL/WebGPU abstraction; gpu_stub.cpp is the no-op default
fiber.h        stackful fibers; fiber_ucontext.cpp (POSIX) / fiber_win.cpp (Windows)
window.h       window/input abstraction; window_sdl.cpp with -DWEBLOX_USE_SDL
CMakeLists.txt optional local build (requires stubs.c generated from WC.h)
```

## Rules

1. Never include `WC.h` without `extern "C"` from C++.
2. Never compile the generated `WC_*.c` as C++ — compile as C, link as C.
3. No hardcoded guest indices, pointers or linear-memory addresses in host
   code. Import stubs come from `gen_stubs.py` reading the generated `WC.h`.
4. Platform-specific code goes behind `window.h` (and future `gpu.h`,
   `net.h`, `fiber.h`) abstractions, not `#ifdef` in `host.cpp`.

## Planned APIs

| Need | API |
|---|---|
| Window/input/audio | SDL3 (`window_sdl.cpp`) |
| WebGL | SDL_GL + GLAD, or ANGLE if semantics diverge |
| WebGPU | Dawn / wgpu-native behind `gpu.h` |
| HTTP | libcurl |
| WebTransport | quiche |
| JSON | nlohmann/json |
| Fibers | Boost.Context (POSIX ucontext / Windows fibers underneath) |

## Build

CI: `gh workflow run heavy.yml -f task=custom -f script=tools/ci/cpp-host.sh`.

Locally (after downloading engine assets with `tools/ci/fetch_engine.sh`):

```sh
python3 tools/ci/gen_stubs.py build/ci/engine/ci-*/WC.h > v3-wasm2c/cpp/stubs.c
cmake -S v3-wasm2c/cpp -B v3-wasm2c/cpp/build
cmake --build v3-wasm2c/cpp/build
```

This skeleton proves the C++ ↔ generated-C boundary works. A join-capable
host requires filling in the abstractions above.
