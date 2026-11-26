# Trap-guard / pthread audit (`feat/cpp-trapguard`)

Scope: bounded native-host investigation in
`/tmp/opencode/wt-cpp-trapguard` (branch `feat/cpp-trapguard`). No Chrome,
Deno or game client was launched. Owned files changed: `host/vmctx.cpp`,
`host/vmctx.h`, `host/unwind.cpp`, plus this note. `host/Makefile` needed no
change. `host/main.cpp`, `host/pthread_start.cpp`, `host/imports.cpp` and
generated files were **not** edited.

## 1. Audit of every `sigsetjmp` / `siglongjmp` site

| Site | jmp_buf / flags | Handler | Verdict |
| --- | --- | --- | --- |
| `host/vmctx.cpp::call_ctors`, `call_malloc`, `call_stack_roundtrip`, `call_trap_test` (`sigsetjmp` at old lines 522/535/549/563) | **Was** process-global `g_trap_jmp`, `g_in_wasm`, `g_trap_signal`; `trap_handler` jumped from whatever thread faulted | `trap_handler` installed process-wide in `setup()` | **Fixed here**: all three are now `thread_local` (`tl_trap_jmp`, `tl_in_wasm`, `tl_trap_signal`). The handler jumps only if `tl_in_wasm` is set on the faulting thread; otherwise it restores `SIG_DFL` and re-raises. A worker can no longer jump into the main thread's frame through this path. |
| `host/pthread_start.cpp::worker_body` (line 256) | Already `thread_local sigjmp_buf tl_jmp`, `tl_active`, `tl_signal` (lines 94–96) | `worker_trap_handler` (lines 98–132) jumps only when `tl_active` on the faulting thread | Already sound for the covered region. Its **fallback** (`g_prev`, lines 121–131) delegates to the process-wide handler that was installed before the worker started. With the fix above, delegation to `vmctx.cpp`'s handler can no longer cross threads. Delegation to `main.cpp`'s handler is still a problem — see §4. |
| `host/main.cpp::ExportTrapGuard::call` (line 104) and `export_trap` (line 82) | **Process-global** `g_export_jmp`, `g_export_active`, `g_export_signal` (lines 75–77) | `export_trap`, installed process-wide by the guard constructor (lines 87–96) | **Not fixed: read-only file.** A signal delivered to a pthread worker while `g_export_active == 1` can `siglongjmp` into the main thread's `sigsetjmp` frame (UB). Exact required change in §4. |

There are no other setjmp/longjmp sites in the tree (`grep -rn 'sigsetjmp\|siglongjmp'
host/*.cpp host/*.h`).

## 2. Changes committed here

1. **`host/vmctx.cpp` — thread-owned trap guard.** `g_trap_jmp`/`g_in_wasm`/
   `g_trap_signal` replaced with `thread_local` equivalents. `trap_handler`
   records the signal and jumps only when the *faulting* thread owns the guard.
   The trace budget is now also `thread_local` (builtins run concurrently in
   workers). `g_trace_enabled` is cached in `setup()` so the handler needs no
   `getenv()`/static-init calls.
2. **`host/vmctx.cpp` / `host/vmctx.h` — `host::reset_wasm_exit_state(void*)`.**
   Zeroes `last_wasm_exit_pc`, `last_wasm_exit_trampoline_fp`, the entry SP/FP
   and `stack_chain[2]` in a vmctx's `VMStoreContext`. A worker that copies the
   main store must call it (§4).
3. **`host/unwind.cpp` — defensive unwinder.** It caches the current pthread's
   native stack range (`pthread_getattr_np`) and refuses to dereference an exit
   FP, frame FP, SP or dynamic-context slot outside that range. It also checks
   that the exit PC maps inside `.text`, and clears stale exit state via
   `reset_wasm_exit_state()`. That turns a copied main-thread PC/FP into an
   honest "no handler / trap" outcome instead of a wild `SIGSEGV` in the FP
   walk.
4. **`HOST_TRACE=1` thread diagnostics.** `trap_handler` writes (with `write()`
   only) e.g.
   `[trap] tid=181688 pthread=0x00007fef37b23fc0 sig=4 pc=0x00005650c2936f84 fault_addr=0x00005650c2936f84 -> thread-local guard: controlled trap`,
   capped at 8 lines per thread. `throw_ref`, `raise` and `trap` builtins log
   `tid=...`, and `unwind_trace()` logs the tid for every throw/rejection.
   Note: traps handled by `main.cpp::export_trap` bypass this handler, so they
   still produce no `[trap]` line — see §4.

## 3. Verification (final tree)

All commands were run in `/tmp/opencode/wt-cpp-trapguard/host`; logs are in
`/tmp/opencode/trapguard_fix_*.log`.

```
make -j4                                            -> demo linked, no compile errors
                                                       (only the pre-existing ld note
                                                       "no .eh_frame_hdr table")
timeout 60s ./demo                                  -> exit 0, RESULT=OK,
                                                       ctors returned normally
HOST_TRACE=1 timeout 60s ./demo --test-trap         -> [trap] tid=... sig=4
                                                       -> thread-local guard:
                                                       controlled trap
WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt \
WC_LOOP_MS=3000 timeout 90s ./demo --rna-probe      -> exit 0, connects=1
                                                       status=200 callbacks=1/0 trap=0
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=... \
WC_LOOP_MS=3000 timeout 90s ./demo                  -> ONE bounded join attempt,
                                                       exit 0, 199 log lines
```

Join evidence (`/tmp/opencode/trapguard_fix_join.log`, verbatim excerpts):

```
[unwind] table: 1122303 callsites, 1547724 handlers at 0x12b341b0 (32040332 bytes)
[trace] throw_ref tid=181565 exnref=0x8
[unwind] throw tid=181565 exnref=0x8 exit_pc=0x55c9b6512eae (rel 0x10979eae) xfp=0x7ffd8ec4a360 fp=0x7ffd8ec4a3b0 code=0x55c9a5b99000
[unwind] FOUND handler pc=0x55c9aea28fc3 sp=0x7ffd8ec4a3c0 fp=0x7ffd8ec4a440
...
[pthr] worker ptr=0x1c618b8 start=152308 trapped via signal 11
...
[pthr] worker ptr=0x2ae96d0 start=417 trapped via signal 11
[import] first call slot=340 env._emscripten_notify_mailbox_postmessage args=0x7f6a062c7010,0xf1e890,0xf1e890,0x7f6906100000,0x7f6906100000
[log ] [WASM-TIMING] OnAppStarted: +3670.4ms from preInit, phase took 0.0ms
[import] first call slot=21 env.emscripten_set_mousedown_callback_on_thread ...
[rna-main] __main_argc_argv: trapped via signal 11
[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
[wasm-host] RESULT=OK
```

Counts for that single join run: 8 `throw_ref tid=...` lines, **13** `FOUND
handler` lines, **0** `no catch-all handler found`, **0** `raise builtin ...
ud2`, 10 `trapped via signal` lines (1 main + 9 pthread workers), **0** fatal
SIGSEGVs and no process abort (exit 0). The earlier documented failure mode
(`throw_ref ... not caught` → `raise builtin -> ud2` → signal 4 in
`host/PTHREAD.md` / `host/GC.md`) did not occur in this run: every throw was
dispatched to a guest landing pad by the hardened unwinder.

**Honest limits.** `main_loop_iteration` still does not run: there is no
`turns=` or `loop turns=` line, and `connects=0`. The final `__main_argc_argv`
fault is now a deterministic controlled trap (signal 11 via the existing
guard), not a crash; the guest simply has not reached the RNA connect. The
`--rna-probe` path proves RNA imports themselves work (`connects=1`,
`status=200`). No worker fault in this run reached `vmctx.cpp`'s handler: all
nine worker traps were contained by the pre-existing thread-local guard in
`pthread_start.cpp`.

## 4. Required edits in read-only files (not attempted)

1. **`host/main.cpp:75–83, 102–109` (`ExportTrapGuard`).** This is the one
   remaining `siglongjmp` that can cross pthreads. Make the buffer and flags
   thread-local and refuse to jump unless the faulting thread owns them:

   ```cpp
   thread_local sigjmp_buf g_export_jmp;
   thread_local volatile sig_atomic_t g_export_active = 0;
   thread_local volatile sig_atomic_t g_export_signal = 0;
   void export_trap(int sig, siginfo_t*, void*) {
     if (!g_export_active) return;          // never jump for another thread
     g_export_signal = sig;
     g_export_active = 0;
     siglongjmp(g_export_jmp, sig);
   }
   ```

   Signal dispositions are process-wide, so worker threads must also install
   their own guard before running guest code (the `SignalScope` in
   `pthread_start.cpp` already does this and its `tl_active` branch jumps only
   locally). With `g_export_active` thread-local, a worker fault outside that
   worker's own guard can no longer longjmp into main's frame. Adding a tids
   log inside `export_trap` would complete the "every controlled trap is
   attributed to a thread" diagnostic.

2. **`host/pthread_start.cpp:168–171` (`make_worker_vmctx`).** After copying
   the main store (`*store = *static_cast<host::VMStoreContext*>(main_store);`)
   call `host::reset_wasm_exit_state(vm);`. Without it a worker store still
   starts with the main thread's exit PC/FP and handler chain. `unwind.cpp` now
   rejects those values defensively, but zeroing them at the source is the
   correct per-thread invariant. `vmctx.h` declares the helper.

3. **`host/imports.cpp:756–758` (`trace_import_args`).** The `backtrace()` /
   `backtrace_symbols()` walk over Cranelift frames (no `.eh_frame_hdr`) was
   identified in `host/MAILBOX.md` §3 as one timing-dependent SIGSEGV trigger
   when `HOST_TRACE=1`. It did not fault in this run, but it is unowned here
   and remains a risk.

## 5. Conclusion

All trap-guard state owned by this task is now strictly thread-local, and the
unwinder no longer dereferences another thread's frame chain. In the bounded
join attempt every observed fault was converted into a controlled trap (exit
0; no fatal SIGSEGV), and native exception propagation now finds guest landing
pads instead of raising SIGILL. The join **still** does not reach
`main_loop_iteration` or fire RNA imports (`connects=0`); that requires the
read-only changes in §4 (plus the mailbox-drain hook documented in
`host/MAILBOX.md` §6). Because item 1 needs `host/main.cpp`, the cross-thread
`ExportTrapGuard` bug is reported rather than claimed fixed.
