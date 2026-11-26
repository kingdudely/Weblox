# `__wasm_call_ctors` debugging findings

## Evidence (all runs used `timeout 30s`)

Baseline: `./demo` timed out with only

```
[import] [pthr] main thread armed; guest thread_init/TLS init requires host hook
```

`gdb -p` samples of the spinning process, repeatedly:

```
#0  _wasm_function_149986
#1  _wasm_function_149790
#2  _wasm_function_149788
#3  _wasm_function_149957
#4  _wasm_function_0     (__wasm_call_ctors)
```

`_wasm_function_149788` iterates a guest cleanup list whose head is the wasm
global at linear address `0x8a13fc`, with sentinel `0x8a13c4`.  The head was
`0`, so the loop treated node 0 as live forever (`malloc(4)`, `free(0)`,
`free(0)` at `_wasm_function_149790`).  A hardware watchpoint confirmed nobody
writes `0x8a13fc`: slot 324 (`_emscripten_init_main_thread_js`) only armed the
mailbox and never called the guest's own thread/TLS init.

## Fixes committed here

1. **Wasmtime 50 wasm-call ABI was modelled wrong.**  `rdi` = callee vmctx,
   **`rsi` = caller vmctx**, wasm args begin at `rdx`, stack arguments are
   callee-popped (`ret $N`).  All 341 import signatures gained a second
   `void* caller_vmctx`; direct calls to wasm exports pass two vmctx values;
   the 6-arg `thread_init` call uses an asm shim (`wb_call_thread_init`).
   Evidence: `HOST_TRACE=1 ./demo` showed slot 324's real pthread_ptr in
   `rdx` (`0xf1e890`), not in `rsi`.
2. **Slot 324 now re-enters the guest**: `_emscripten_thread_init(ptr,1,1,1,
   262144,0)`, `_emscripten_tls_init()`, `pthread_self()` — same sequence as
   `v2-wasmtime/src/imports/boot.rs` and `v3-wasm2c/harness.c`.
3. **VMContext runtime data** was unallocated/uninitialised.  It needs
   `3689 * (8+4)` extra bytes for one base+length per wasm data segment.
   `host/gen_runtime_data.py` emits all segment images from the wasm binary;
   `vmctx.cpp` fills the arrays, copies active segments into linear memory,
   and makes `memory.copy/fill` use absolute pointers (Cranelift already
   bounds-checked them).
4. **`emscripten_resize_heap`** acknowledges growth (memory is already 4 GiB)
   instead of returning 0 and driving dlmalloc into its abort path.
5. **The wasm start function (index 344, `_wasm_function_3`) is now called**
   before `__wasm_call_ctors`.  This materialises passive data segments
   (including the thread-registry image containing the `0x8a13c4` sentinel).
   The original cleanup spin is gone.

## Blocker resolved: element-section table initialisation

After the fixes above, `HOST_TRACE=1 ./demo` progressed through slots 324, 341,
325, 309, 310 and then trapped with SIGILL at
`_wasm_function_150264+2214` (called directly from `_wasm_function_0+717`).
`_wasm_function_150264+515` is a `call_indirect` type check: it loads a
`VMFuncRef` from `__indirect_function_table` (`vmctx+0x2ae8`) and compares its
`type_index` (offset 16) with the expected value.  gdb at `+513` showed
`edx = 1` (our placeholder funcref's type index) and `ecx = 0` (expected).
The expected value is `type_ids[module_type_index]`, loaded from the VMContext
at offset 40 — the host had zeroed that whole array, so *every* expected type
was 0.  The placeholder also had `wasm_call == nullptr`.

Fix (this branch):

* `host/gen_table.py` parses the wasm table and element sections and generates
  a small checked-in `table.h` plus a build-time-only `table.cpp` (16 MB,
  gitignored).  `make` regenerates it from
  `/tmp/opencode/WebClient.raknet.wasm`.  Guest symbols are resolved by direct
  `extern "C"` references because Cranelift emits them as GLOBAL **HIDDEN** in
  the object, so `dlsym(RTLD_DEFAULT, "_wasm_function_N")` cannot see them in
  the linked executable (`nm`/`readelf` confirm `LOCAL`/`HIDDEN`).
* `vmctx.cpp` builds one real `VMFuncRef` per active element: `wasm_call` =
  guest function address, or `wb::import_wasm_call(func_index + 1)` for the 5
  imported functions stored in the table; `type_index` = wasm type index;
  `vmctx` = instance vmctx.  Table index 0 stays null (the segment's offset is
  1).  `type_ids[i] = i` gives the generated code a consistent identity
  mapping on both sides of the `call_indirect` check.

Exact evidence for this build:

```
table section:  1 table, funcref, min=max=161491
element section: 1 segment, flags=0 (active, implicit table 0), offset=1,
                 count=161490 (func indices 51..151424; 5 imports, 161485 defined,
                 94438 distinct defined functions)
host log:       [wasm-host] table: 161491 entries, 161490 initialized
```

Bounded verification (`timeout 30s`, after `make`):

* `./demo` — exit 0, `__wasm_call_ctors (_wasm_function_0): returned normally`,
  `RESULT=OK`, no trap, `controlled errors=0`.
* `./demo --skip-ctors` — exit 0, `RESULT=OK`.
* `HOST_TRACE=1 ./demo` — exit 0; imports 49, 50, 40, 4, 317, 320, 7 are
  exercised and ctors still returns normally.

Generation is deterministic: regenerating `table.cpp`/`table.h` twice produces
identical SHA-256 hashes.  Only the generator and the 590-byte `table.h` are
committed.
