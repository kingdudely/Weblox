# Main-thread SIGSEGV after slot 21 — root cause

## Bottom line

The fault is **not** caused by slot 21's return value or by missing callback
registration. It is a **wasm-call ABI mismatch in the generated import stubs**:
guest code calls the import table expecting the callee to pop stack arguments
(`ret $imm16`), but every generated stub in `host/imports.cpp` ends with a
plain System-V `ret`. The first 5-argument import reached during bootstrap is
slot 21; its stub does not pop, so the guest's `%rsp` drifts by 16 bytes and the
very next import call loads a bogus import pointer (zero) and jumps to address
0.

Fixing this requires editing the stub generator / generated stubs
(`host/gen_imports.py`, the `=== BEGIN GENERATED STUBS ===` region of
`host/imports.cpp`, `host/imports.h`) — all read-only for this task. Scope was
limited to slots 20/21 and slot 15, so the SIGSEGV was **not** fixed here.

## Reproduction

In `host/`:

```
make
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt \
  WC_LOOP_MS=3000 timeout 60s ./demo
```

Unchanged baseline (`/tmp/final-join.log`, lines 194–197):

```
[import] first call slot=20 env.install_pointer_source_tracking ... ret=...
[import] first call slot=21 env.emscripten_set_mousedown_callback_on_thread ... ret=...
[rna-main] __main_argc_argv: trapped via signal 11
[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
```

Reproduced identically after this pass (`run-final1.log`, lines 195–200): same
trap, no `main_loop_iteration` turns, `connects=0`, `[wasm-host] RESULT=OK`.

## Faulting PC / signal

`HOST_TRACE=1` does not print `[trap]` here: `main.cpp`'s `ExportTrapGuard`
replaces `vmctx.cpp`'s tracing handler. A bounded `ptrace` tracer
(`/tmp/opencode/trace_fault.c`, stops at the first SIGSEGV and reads
`PTRACE_GETREGS`) gives, for the main thread:

```
[PTRACE] tid=188376 sig=11 rip=0 rsp=7fff0fd77728 rbp=7fff0fd78140
         rdi=0 rsi=7fa81c5f24e0 r10=0
```

`sig=11`, `rip=0`, `r10=0`, `si_code=SEGV_MAPERR`, `si_addr=0`,
`err=0x14` (user-mode instruction fetch from a non-present page): the guest
executed `call *%r10` with `r10 == 0`. `rsi` (the import's `caller_vmctx`) is
`0x7fa81c5f24e0`, whereas the correct vmctx at slot 21 was `0x7fa81ae72010`
— the guest read a stale/garbage stack slot instead of its vmctx spill.

GDB could not be used: attaching or loading a core dump fails with
`Recursive internal problem` on this host (libthread_db). The `ptrace` tracer
and the core-file NT_PRSTATUS note were used instead.

## Why `r10` is zero

Guest code is `_wasm_function_147` (`__main_argc_argv`), offset
`0x95b..` in `/tmp/opencode/out.full.o`. Immediately after the slot-21 call:

```
95be59: mov    %eax,(%rsp)          # 5th wasm arg (thread id = 2)
95be5c: call   *%r10                # slot 21, wasm_call @ vmctx+0x2d0
95be5f: sub    $0x10,%rsp           # allocate stack args for NEXT import
95be63: mov    $0x1,%edx            # slot 22 setup ...
95be7b: mov    0x28(%rsp),%rsi      # guest vmctx spill slot
95be87: mov    0x2f0(%rsi),%r10     # slot 22 wasm_call
95be91: call   *%r10                # <-- fault: r10 == 0
```

The same `sub $0x10,%rsp` follows each successive 5-argument registration
(0x95be94, 0x95bec9, 0x95befe, ...). That pattern means the guest expects the
callee to have popped the 16 bytes of stack arguments: after a correct
callee-pop, the `sub` restores `%rsp` to its prior value. The generated host
stub instead ends with a plain `ret`:

```
45f05e: ret                         # wasm_import_021_... (plain SysV epilogue)
```

With a plain `ret`, `%rsp` is 16 bytes too low after slot 21; the next
`0x28(%rsp)` load no longer reads the vmctx, so `r10` becomes 0 and the call
jumps to address 0.

### Decisive experiment

I patched a **copy** of the linked binary (`/tmp/opencode/demo-calleepops`,
not committed) changing only slot 21's final `c3` to `c2 10 00`
(`ret $0x10`) — i.e. giving that one stub the expected callee-pop epilogue.
Result (`run-calleepops.log`, lines 72–76):

```
[import] first call slot=20 env.install_pointer_source_tracking
[import] first call slot=21 env.emscripten_set_mousedown_callback_on_thread
[import] first call slot=22 env.emscripten_set_mouseup_callback_on_thread
[PTRACE] tid=189027 sig=11 rip=0 rsp=7ffc827a5758 r10=0
```

Slot 22 now runs, and the identical null-call fault moves to the *next*
5-argument import. That confirms the mismatch is systemic: slots 21–35 (and
every other import with native parameters beyond the six register args) need
the wasm-call callee-pop epilogue. It is not a slot-21 semantic issue.

## Changes made in this pass (in scope)

### Slot 21 — honest registration, no re-entry

Guest call arguments at `95be2e..95be59` map to Emscripten's
`emscripten_set_mousedown_callback_on_thread(target, userData, capture,
callbackfunc, thread)`:

```
a0 = 0xe2b50 (canvas target id), a1 = 0 (userData), a2 = 1 (capture),
a3 = 0x11 = 17 (callback table index), a4 = 2 (thread)
```

`wasm_import_021_...` now stores target/user/capture/callback/thread in a
process-lifetime atomic table and returns 0. This matches
`v2-wasmtime/src/imports/input.rs` (`set!` macro stores CB[id]/USER[id],
returns 0) and `emscripten_set_*_callback_on_thread` semantics. Nothing is
faked and there is no wasm re-entry from a host thread; no synthetic mouse
event is delivered because no input source exists. This change alone does not
alter the fault (expected).

### Slot 20 — unchanged

`install_pointer_source_tracking` is a no-op; `is_mouse_sourced_event` (slot
12) already returns mouse (1), consistent with
`v2-wasmtime/src/imports/stubs.rs` (`pointer_type = 0`, default mouse).

### Slot 15 — real 1280×720, now as f64

`emscripten_get_element_css_size(const char*, double*, double*)` writes
**double** out-params. The previous stub wrote `float` 1280/720 (as the v2
stub does), so the guest's `vmovsd` read a ~0 double and logged
`Canvas: 0x0 CSS, 0x0 backing`. After changing to `double`:

```
[log ] Canvas: 1280x720 CSS, 1280x720 backing (effective dpr=1.00, raw=1.00)
```

This was a real correctness bug but not the SIGSEGV: the fault still occurs
with it fixed.

## Verification after the in-scope changes

* `make` — succeeds (only the pre-existing `.eh_frame_hdr` linker warning).
* `timeout 60s ./demo` with the task's environment — reaches `OnAppStarted`,
  gamepad imports, slots 14–21, then `[rna-main] __main_argc_argv: trapped via
  signal 11`. No `main_loop_iteration` turns, no RNA connect:
  `final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=`.
* Bounded join: not attempted beyond the single reproduction; the guest never
  returns from `__main_argc_argv`, so no loop iterations or RNA imports can
  fire.
* Canvas log now shows real 1280×720 values.

## What must change outside this scope

Generate wasm-call import stubs with the callee-pop epilogue
(`ret $imm16`, where `imm` is the byte count of stack arguments — 16 for the
5-wasm-arg slots 21–35), or route imports through a trampoline compiled with
that ABI. The generator is `host/gen_imports.py`; its output is the generated
stub region of `host/imports.cpp` and the declarations in `host/imports.h`.
Until that is fixed, bootstrap will fault at the first multi-argument import
after the fixed canvas block, regardless of what slots 20/21 return.
