# Native AOT feasibility findings (2026-11-26)

Recorded so future agents do not repeat these experiments. All tests used the
**patched** engine module `/tmp/opencode/WebClient.raknet.wasm`
(102,267,785 bytes) produced by `v2-wasmtime/tools/patch_new.py --all` from
`version-1ef59424f70d4932`. The module has 342 imports (1 shared memory +
341 functions), 154,552 functions, shared memory + atomics, SIMD, exception
handling (`exnref`, `try_table`), tail calls and bulk memory.

## Verdict

**Cranelift-object wins.** Wasker and w2c2 are ruled out. The emitted native
object needs no Wasmtime runtime engine — only a thin C++ host.

## 1. Wasker — ruled out

Branch `feat/wasker`, commit `3de5d18`. Full evidence in
`docs/WASKER-FEASIBILITY.md` on that branch; reproduction script
`tools/wasker-feasibility.sh`.

- Prebuilt v0.1.1 linux-x86_64: 43,268,976 B, sha256 `c816537f…437a3`.
- Command: `wasker /tmp/opencode/WebClient.raknet.wasm` → fails in **~1 s**:
  ```
  Error: TypeSection: Type::ArrayType unimplemented
  ```
- Real cause: wasmparser 0.102 rejects `exnref` result types (type #12 is
  `0x69`). The error message is a catch-all.
- Further blockers even after removing `exnref`: imported shared memory
  (`unreachable!("ImportSection only support Func type")`), `v128` SIMD
  (`unimplemented ValType: V128`), EH tags/opcodes, tail calls, `memory.init`/
  `data.drop`.
- Control module confirmed the good property: wasker emits `.ll` + ELF
  relocatable `.o` with imports as **undefined symbols**, and a 15-line C++
  `main.cpp` links. But that cannot apply to this module.

## 2. w2c2 — ruled out

Branch `feat/w2c2`, commit `a5e93b7`. Full evidence in `docs/W2C2-TIMING.md`
and four helper scripts on that branch.

- **Hard correctness blocker:** no exception-handling or tail-call opcode
  support. Module has 1 `exnref` type, **42,301 exnref locals**, `exnref`
  block types and `try_table`/`catch_all_ref`. Binaryen v120 `--strip-eh`
  fails validation unless `--no-validation`, and still leaves exnref locals.
- Volume numbers, only after a semantic `--strip-eh --no-validation` hack and
  rewriting those locals (NOT a usable port):
  - translation `-f 512 -t 8`: **6.79 s**, peak RSS 153 MB
  - 304 files / **688 MiB** of C (302 split files ~2.65 MB each,
    `module.c` 56 MB)
  - `clang -std=c99 -O0` + SIMDe, `xargs -P4`: **4,665 s (77.75 min)**,
    peak aggregate clang RSS 4,041 MB with swap pressure
  - 303 objects (757 MB), `module.a` 759 MB
  - incremental rebuild of one TU: **6.22 s** (but regeneration defeats ccache)
  - after `ld -r`: **359 undefined symbols** = 341 wasm imports + 18
    libc/runtime (`calloc`, `sqrt`, `trap`, `wasmMemoryAtomicWait`, …)
- A separate live check found 302/303 objects done but one 23 MB TU compiling
  for **69 minutes** at 94% CPU / 1.8 GB RSS — the practical meaning of
  "3 GB C parse takes forever".
- Verdict: translator speed and output size are fine, but EH incompatibility
  is fatal and a 78-minute full build is not a workable weekly loop.

## 3. Cranelift-object — selected

Branch `feat/cranelift-object`, commit `110da3a`. Tool:
`tools/wasm2obj/` (`src/main.rs`, `Cargo.toml`, `Cargo.lock`, `README.md`).

- Uses the **vendored Wasmtime 50 compiler APIs** (`compile_function`,
  `compile_trampoline`, `object()`, `append_code`) from `wasmtime-internal-*`
  crates, NOT the standalone `cranelift-wasm` crate. The standalone crate's
  last release is 0.112.3 and has no `try_table` support.
- All features accepted with `WasmFeatures::WASM3 | SHARED_EVERYTHING_THREADS |
  LEGACY_EXCEPTIONS`: threads/atomics, SIMD, exnref/`try_table`, tail calls,
  bulk memory, shared memory. Zero translation rejections.
- Required fix: `Tunables::default_host()` has `collector: None`, which panics
  with `Unsupported("support for GC types disabled at configuration time")`;
  set `Collector::DeferredReferenceCounting`.
- Timings (release tool, 8 cores, rayon):
  | phase | time |
  | --- | --- |
  | cold tool build (all deps) | 3 m 50 s |
  | read wasm | 0.14 s |
  | parse + validate + translate | 0.118 s |
  | compile 154,552 functions | 250.2 s |
  | builtins + `append_code` + write | 9.0 s |
  | **total** | **261.6 s** |
  Sequential codegen was killed at 39k/154k after 300 s — rayon is required.
- Output `/tmp/opencode/out.full.o`: **342,188,600 B**,
  `ELF 64-bit LSB relocatable, x86-64, not stripped`. Sections include
  `.text`, `.eh_frame`, `.wasmtime.stackmap`, `.wasmtime.traps`,
  `.wasmtime.exceptions`.
- 154,560 global (`T`) symbols: 154,552 `_wasm_function_N` plus 8
  `wasmtime_builtin_*` trampolines (`memory_copy`, `memory_fill`,
  `memory_atomic_wait32`, `memory_atomic_notify`, `throw_ref`,
  `table_get_lazy_init_func_ref`, `gc_alloc_raw`, `get_instance_id`).
  **0 undefined symbols, 0 relocations** — imports are called via VMContext
  pointers, so the linker cannot satisfy them; the host must populate them.
- Export mapping: wasm func index − 341 = `_wasm_function_N`; e.g. `malloc`
  (export func 150325) → `_wasm_function_149984`.

### What the C++ host must supply

1. VMContext layout from vendored `vmoffsets.rs`, `vmctxtypes.rs`, `vmtypes.rs`.
2. 341 `env.*` import function pointers (embind, fetch, pthreads, GL, audio).
3. The 8 builtin trampolines.
4. Guarded linear memory (`mmap` ~4 GB + guard pages) and `memory.grow`.
5. Signal handlers converting traps to wasm traps.
6. Export mapping to call functions directly from `main`.

No Wasmtime `Engine`, `Store`, linker or JIT is needed at runtime. The build
still depends on the vendored Wasmtime crates as a library.

## Open decision

Transport for the C++ host. A short **CEF oracle** (real Chromium running the
stock Roblox page with the session cookie) will decide:
- session survives >120 s → WebTransport viable; implement with native
  quiche/ngtcp2 in C++;
- session dies at ~110 s → use native UDP + RakNet (simpler in C++).
