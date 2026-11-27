# Worker trap guard (`feat/cpp-worker-guard`)

Scope: bounded native-host work in
`/tmp/opencode/wt-cpp-worker-guard` (branch `feat/cpp-worker-guard`). No Chrome,
Deno or game client was launched. Changed files: `host/pthread_start.cpp`,
`host/pthread_start.h`, `host/unwind.cpp`, plus this note. `host/Makefile`
needed no change. `host/main.cpp`, `host/vmctx.*`, `host/imports.*`,
`host/loop.*`, `host/wt.*`, `host/route.*`, `host/rna_bridge.*`,
`host/prepare.*` and generated files were **not** edited.

## 1. Failure evidence (baseline, `feat/cpp-abi`)

One bounded join with `HOST_TRACE=1 WC_PLACE=189707
WC_COOKIE_FILE=/home/john/mizg.txt WC_LOOP_MS=3000 timeout 90s ./demo` produced
`/tmp/join-abi.log` (23653 bytes). Verbatim key lines:

```
234: initialize returned, code: 0
239: [import] first call slot=8 env.schedule_main_loop args=...,0x30,0 ret=...
241: [rna-main] __main_argc_argv: trapped via signal 4
242: [rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
243: [wasm-host] import calls=0 controlled errors=0
244: [trap] tid=... sig=11 pc=... fault_addr=... -> no guard on this thread: re-raise
     (four concurrent, byte-interleaved copies; one per surviving pthread)
```

Process exit was **139 (SIGSEGV)**. The log contains nine
`[pthr] worker ... trapped via signal 4` lines: those traps were contained by
the old `pthread_start.cpp` guard, but the four later faults reached
`vmctx.cpp::trap_handler` with `tl_in_wasm == 0`; that handler restores
`SIG_DFL` and re-raises, killing the whole process.

Root cause: signal dispositions are process-wide. `main.cpp`'s
`ExportTrapGuard` destructor, `imports.cpp`'s drain guard and `loop.cpp`'s hook
guard install and restore handlers without knowing that detached workers are
still executing guest code. The old worker guard was installed only around the
guest-call window (`SignalScope` in `worker_body`) and restored on exit. Once a
later guard restored `vmctx.cpp`'s re-raising handler, any fault on a worker
was fatal. An intermediate experiment with a re-installer thread failed too:
the guard restore and the faults happen in the same sub-millisecond window, so
polling cannot close it.

## 2. Changes

1. **`host/pthread_start.cpp` — lifetime-long, thread-owned guard.** Each
   worker's `std::thread` entry now sets `tl_is_worker`/`tl_tid` (via
   `SYS_gettid`) and installs `sigsetjmp(tl_outer_jmp, 1)` **before**
   `worker_body()`, keeping that frame valid for the whole thread (including
   vmctx allocation, cleanup and teardown). `worker_body()` keeps the narrower
   `tl_inner_jmp` around the guest-call window so cleanup can still run after
   a trap. The handler covers `SIGSEGV`, `SIGFPE`, `SIGILL` and `SIGBUS`; it
   logs one atomic `write(2)` line with tid, signal and PC, then `siglongjmp`s
   to the inner or outer buffer **on the faulting thread only**. It never
   restores `SIG_DFL` or re-raises for a worker. If neither jmp target is live,
   it ends only that pthread with `pthread_exit` rather than the process.
2. **`host/pthread_start.cpp` — race-free handler retention.** From
   `pthread_register()` onward, `sigaction()` for those four signals is
   interposed (real function resolved with `dlsym(RTLD_NEXT, "sigaction")`).
   A foreign action (from the read-only guards in `main.cpp`, `imports.cpp`,
   `loop.cpp`) is *recorded* as the fallback for the main thread but is never
   installed; the caller receives our current action as `old`, so its own
   destructor restores ours. Fallbacks are a stack so nested guards unwind
   correctly (without that, a finished `HookGuard` stayed as main's fallback
   and produced a second, fatal fault at `__main_argc_argv`). Non-worker
   threads are delegated to the recorded action verbatim, so main's
   `ExportTrapGuard` still converts guest traps to controlled errors.
3. **`host/unwind.cpp` + `host/pthread_start.h` — no unguarded `ud2` on a
   worker.** Every failure return of `unwind_throw()` (no exception table, no
   exit state, stale main-thread FP, invalid PC, no catch-all handler) now
   calls `wb::worker_raise_controlled(SIGILL)`. On a worker that logs the tid
   and jumps into this thread's guard; on main it is a no-op and the original
   `builtin_raise -> ud2` trap path is preserved. This removes the
   `throw_ref -> raise -> ud2 -> SIG_DFL` route that previously killed workers.

## 3. Verification (final tree)

All commands were run in `/tmp/opencode/wt-cpp-worker-guard/host`; logs are in
`/tmp/opencode/worker-guard-final-*.log`.

```
make -j4                                            -> exit 0 (only the known
                                                       ld note "no .eh_frame_hdr
                                                       table will be created")
timeout 60s ./demo                                  -> exit 0, RESULT=OK
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=... \
  WC_LOOP_MS=3000 timeout 90s ./demo --rna-probe    -> exit 0,
  [rna-main] probe ticks=592 trap=0 connects=1 status=200 callbacks=1/0
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=... \
  WC_LOOP_MS=3000 timeout 90s ./demo                -> ONE bounded join, exit 0
```

Join evidence (`/tmp/opencode/worker-guard-final-join.log`, 258 lines,
verbatim excerpts):

```
239: initialize returned, code: 0
244: [import] first call slot=8 env.schedule_main_loop args=...,0x30,0 ret=...
246: [rna-main] __main_argc_argv: trapped via signal 4
247: [rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
258: [wasm-host] RESULT=OK
```

Counts for that single join: **9** `[pthr-guard] tid=... sig=11 pc=... ->
controlled worker trap (no re-raise)` lines, **9** matching
`[pthr] worker tid=... ... contained by worker guard` lines, **13** `FOUND
handler` lines, **0** `no catch-all handler found`, **0** `raise builtin ...
ud2`, **0** `no guard on this thread: re-raise`, **0** fatal SIGSEGVs. Exit was
**0**, not 139. A final rebuild and second bounded join
(`/tmp/opencode/worker-guard-final2-join.log`) reproduced the same counts
(9 controlled worker traps, 0 re-raises, 13 `FOUND handler`) and exit 0.

**Honest limits.** `main_loop_iteration` still does not turn: the driver prints
no `turns=`/`loop turns=` because `__main_argc_argv` itself traps with signal 4
(line 246), so the loop branch is never entered and `connects=0` in the join.
The `--rna-probe` run proves RNA imports work independently (`connects=1`,
`status=200`, `callbacks=1/0`). Getting the join to reach the loop is a guest
bootstrap/ABI problem outside the files owned by this task; this change only
makes worker traps non-fatal and attributable. The `sigaction` interposition
is deliberately limited to `SIGSEGV/SIGFPE/SIGILL/SIGBUS` and to the window
after `pthread_register()`; it does not alter any other signal or any other
host file.

## 4. Conclusion

The exit-139 failure mode is gone: every worker fault in the bounded join was
converted to a controlled, tid-attributed error on its own thread, and the
process finished with `RESULT=OK`. Exception dispatch still finds guest
landing pads (13 `FOUND handler` lines), and the `throw_ref` no-handler path
now diverts workers away from `ud2`. RNA imports fire in `--rna-probe` but not
yet in the full join, where `__main_argc_argv` still traps before
`main_loop_iteration`.
