# Pthread worker re-entry in the C++ host

## Problem

The old `main.cpp::thread_entry()` did

```cpp
snprintf(sym, "_wasm_function_%u", start - 341);
dlsym(RTLD_DEFAULT, sym);
```

and failed for **every** spawn. A bounded join baseline
(`/tmp/opencode/wt-cpp-assets-join2.log`) shows nine warnings:

```
[pthr] no wasm symbol for start=152308 (_wasm_function_151967)
[pthr] no wasm symbol for start=417 (_wasm_function_76)   x9
```

Two independent reasons:

1. `start` is a **table index**, not a wasm function index. The generated
   element table (`host/gen_table.py` → build-time `table.cpp`, 161,491
   entries, 161,490 initialized) resolves these to *different* functions:

   | start (table index) | func index | defined index `N` |
   | --- | --- | --- |
   | 417 | 853 | `_wasm_function_512` (not `76`) |
   | 152308 | 134416 | `_wasm_function_134075` (not `151967`) |
   | 152880 | 139219 | `_wasm_function_138878` |

2. Cranelift emits `_wasm_function_N` as GLOBAL **HIDDEN** in `out.full.o`;
   linking makes them LOCAL, so `dlsym(RTLD_DEFAULT, ...)` can never see them
   (see `host/CTORS_FINDINGS.md`). The resolved pointers must be bound by
   direct `extern "C"` references (as `vmctx.cpp` already does for the table).

## What changed

* **`host/pthread_start.cpp` / `.h`** (new): the `ThreadEntry` hook installed
  by `main.cpp`. It looks `start` up in a registration-time index built over
  `wb_table_init[]` and calls the entry's `wasm_call` pointer from `table.cpp`,
  with `(callee_vmctx, caller_vmctx, arg)` per the Wasmtime 50 wasm-call ABI.
  No `dlsym` remains in this path.
* **Per-thread VMContext**: the worker copies the whole instance block
  (including the 3,689 runtime-data base/length pairs), then rebases every
  self-pointer: private `VMStoreContext`, imported function slot vmctx fields,
  imported-memory vmctx, all 94,841 escaped `VMFuncRef`s and the startup
  funcref. It builds its own indirect table from `wb_table_init[]` so
  `call_indirect` in a worker carries the worker's vmctx. Mutable wasm globals
  1–7 are zeroed, matching a freshly instantiated worker; global 0 (stack
  pointer) is set from the pthread descriptor (`+48` top, `+52` size) via the
  guest's `set_stack_limits` / `set_stack_ptr` exports, exactly as Rust's
  `v2-wasmtime/src/imports/thread.rs` and `v3-wasm2c/thread.c` do.
* **TLS init order** mirrors the browser worker and Rust: `_emscripten_thread_init`
  (own asm shim, 6 wasm args, `is_main = 0`), `_emscripten_tls_init`,
  `pthread_self`, `_embind_initialize_bindings`, then the table-selected entry.
* **Trap containment**: a thread-local `sigsetjmp`/handler catches faults in
  worker code and delegates all other signals to `vmctx.cpp`'s handler, so a
  worker fault cannot longjmp into the main thread's frame.
* `main.cpp` only swaps the hook (`wb::pthread_entry`) and registers the main
  vmctx; `Makefile` compiles the new translation unit. `table.cpp` remains a
  gitignored build artefact regenerated from the wasm file.
* `WC_PTHR_DEBUG=1` prints bounded worker phase diagnostics.

## Verification (bounded, one join attempt)

After `make`:

* `timeout 60s ./demo` — exit 0, `_wasm_call_ctors returned normally`,
  `RESULT=OK` (`host/verify_demo_final.log`).
* `WC_PLACE=189707 WC_COOKIE_FILE=... timeout 90s ./demo --rna-probe` — exit 0,
  `connects=1 status=200 callbacks=1/0 trap=0`; RNA imports do run
  (`host/verify_rna_probe_final.log`). Without a cookie this mode fails early
  with `empty .ROBLOSECURITY cookie`, which is expected.
* **One** `HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=...
  WC_LOOP_MS=3000 timeout 90s ./demo` join attempt
  (`host/verify_join.log`, exit 0, 100 lines):
  * **no `no wasm symbol` warnings at all** (baseline: 9);
  * the engine now progresses through `initialize()` (+1535 ms),
    `RunDynamicInitializers`, `CrashReporter init`, `gameGlobalInit`,
    `LoadFlags`, spawns thread #1 (start=152308) and nine more (start=417);
  * `throw_ref exnref=0x8 -> not caught` **still fires**, followed by
    `raise builtin -> ud2`, `__main_argc_argv: trapped via signal 4`. This is
    the pre-existing DRC exception-propagation limit (`host/GC.md`), not a
    pthread regression. Because of it `final: connects=0` — the join path never
    reaches the RNA connect in this build; the `--rna-probe` above proves the
    RNA imports themselves work.
  * first worker reports `trapped via signal 11`; the nine start=417 workers
    do not finish inside the 3 s loop.
* Deterministic out-of-tree harness (no network, engine bootstrap skipped)
  that calls `wb::pthread_entry()` with a synthetic pthread descriptor and
  table slot 1131 (`_wasm_function_43`, a no-op): all init phases complete,
  `pthread_self` returns the synthetic pointer, and the log ends with
  `[pthr] worker ... -> func=384 (called) tls=0x10000000 done`. This proves the
  per-thread vmctx/TLS/table path itself is sound.

## Remaining limits

* Real start routines (152308, 417) still fault when run against this host's
  partially bootstrapped engine state. The C++ host has no native wasm
  exception unwinder (`throw_ref` returns "not caught"), no fiber/event-loop
  model, and a single shared GC heap/import state instead of one Wasmtime
  `Store` per worker. Fixing those is outside this change.
* Workers are detached and terminate after the entry returns/traps; Rust's
  `start_worker()` keeps pooled workers alive and pumps their mailboxes. The
  mailbox primitives already exist in `imports.cpp` but are not exposed as a
  worker pump yet.
* `start` resolution uses the active element segment only. A passive segment
  materialised later by the guest (`table.init`) would need a dynamic lookup.
* The generated `table.cpp` (16 MB) and `runtime_data.cpp` are build artefacts,
  regenerated by `make` from `/tmp/opencode/WebClient.raknet.wasm`.
