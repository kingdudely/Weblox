# `emscripten_exit_with_live_runtime` (slot 9) in the native host

## What changed and why

In a browser/Deno host, `env.emscripten_exit_with_live_runtime` unwinds out of
`main()` but leaves the JS runtime alive; the page then pumps
`main_loop_iteration` from its event loop.  The previous native stub simply
returned from the void import, so `__main_argc_argv` fell through into guest
code that issued an uncaught `throw_ref` (`SIGILL`, reported as
`__main_argc_argv: trapped via signal 4` in `/tmp/join-abi.log`).

This branch makes slot 9 an honest live-runtime exit:

* `host/imports.cpp` (slot 9 body only) sets `wb::imports_live_runtime()` and a
  call counter, logs `[live] slot 9 ...` under `HOST_TRACE`, and transfers
  control to a continuation registered on the calling thread by the join
  driver.  The continuation is a `siglongjmp` into the driver's own
  `sigsetjmp` frame; **no C++ exception crosses a wasm frame**, no wasm value
  is fabricated, and the import stays `void`.  A call from any other thread
  cannot use that continuation: the registration records the owner
  `std::thread::id`.  Without a registered continuation (ctors, probe, etc.)
  the old "return normally" behaviour is preserved.
* `host/main.cpp` (join driver only) registers
  `ExportTrapGuard::live_runtime_unwind` immediately before the guarded
  `_wasm_function_147` call and clears it immediately afterwards.  The
  continuation reports the pseudo-signal `kLiveRuntimeReturn = -9` (negative
  so it can never collide with a real `SIG*` number), sets
  `g_export_active = 0` before jumping, and makes `guard.call()` return -9.
  The driver prints `__main_argc_argv: live runtime (slot 9): main returned,
  runtime alive`, clears the registration, and — because this is not a fatal
  trap — calls `weblox::run_loop(state, deadline, ...)` on the same turn as a
  normal `main()` return.
* `host/loop.cpp` now owns that bounded driver.  `weblox::run_loop()` walks the
  deadline, invokes the caller-supplied guarded turn, counts attempts, sleeps
  2 ms between turns, and reports RNA stats every 100 turns.  Every turn still
  resolves to `__wrap__wasm_function_176` (Makefile already links
  `-Wl,--wrap=_wasm_function_176`), which pumps one RNA completion, drains
  mailbox rings, calls the real `main_loop_iteration`, and drains again.
  With `HOST_TRACE=1` the first three turns log `[loop] turn N begin/end` so a
  stalled turn is visible.

`host/imports.h` and all other files were left untouched.

## Verification (final build, branch `feat/cpp-liveruntime`)

All commands ran in `/tmp/opencode/wt-cpp-liveruntime/host`.

### 1. `make -j4`

Exit 0.  Recompiled `main.cpp`, `imports.cpp`, `loop.cpp`; only the pre-existing
`/usr/bin/ld: error in /tmp/opencode/out.full.o(.eh_frame); no .eh_frame_hdr
table will be created` linker diagnostic remains.

### 2. `timeout 60s ./demo`

Exit 0.  Tail of `/tmp/live-demo-final.log`:

```
[wasm-host] __wasm_call_ctors (_wasm_function_0): returned normally
[wasm-host] import calls=0 controlled errors=0
[wasm-host] malloc(16) (_wasm_function_149984): returned 27189176 (0x19edfb8)
[wasm-host] RESULT=OK
```

### 3. `timeout 90s ./demo --rna-probe`

Exit 3, the expected pre-network failure (no cookie supplied in this command):

```
[rna-main] join attempt failed: empty .ROBLOSECURITY cookie
```

### 4. One bounded join

```
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt WC_LOOP_MS=3000 timeout 90s ./demo
```

Exit 124 (`timeout` killed it at 90 s).  Full log: `/tmp/live-join-final.log`.
Exact key lines (line numbers from that file):

```
32:  [rna-main] route: fqdn=474098bcc...qdmux.roblox.com host=128.116.47.34 port=50551 caps=120ddbc750b31896 cert=2D/8+TOF... place=189707 status=2
45:  [rna-main] prepare ok=1 trap=0 meta=data/meta.json content=data/content
46:  [rna-main] __main_argc_argv argc=1 argv=27604480 (argv_trap=0)
185: [drain] main=0xf1e890 readied=1 pending=0 guest_calls=1 traps=0
241: [live] slot 9 vmctx=0x7fd9d4796010 caller=0x7fd9d4796010 main-return, keep runtime
242: [rna-main] __main_argc_argv: live runtime (slot 9): main returned, runtime alive
243: [loop] run_loop entered (wrapped iteration)
244: [loop] turn 1 begin
245: [drain] main=0xf1e890 readied=1 pending=0 guest_calls=2 traps=0
```

Then the guest itself logs `launchUGCGame`, slot 3 `js_fetch_file` (both
requested asset files exist under `data/content`; the 252-byte config matches
the request's `want=0xfc`), `Hello World! CLI-232246`, and slot 53
`emscripten_asm_const_int_sync_on_main_thread`.  The log ends there.

Answers to the verification questions:

* **Slot 9 handled correctly?** Yes.  It is no longer a fatal trap: the old
  `__main_argc_argv: trapped via signal 4` line is gone, replaced by the
  explicit `live runtime (slot 9): main returned, runtime alive` line.  No
  `[trap]` line appears after slot 9 in this run.
* **Loop turns run?** The loop driver is entered and **turn 1 begins**, but
  **no turn completes**: neither `[loop] turn 1 end` nor the final
  `[rna-main] loop turns=... iter_trap=...` line was printed before the 90 s
  timeout.  Guest-side `launchUGCGame` proves the real `main_loop_iteration`
  was entered on that turn.
* **RNA `connect_js` / connects?** Slot 96 was not reached.  No progress line
  was printed (those are every 100 turns and no turn completed), and no
  `[rna-main] final:` line was reached, so `connects=` and CONNECT status were
  never reported.  The `status=2` on the route line is the gamejoin route
  fetch status, **not** an RNA CONNECT result.
* **Remaining traps?** None observed after slot 9 in this run; in particular
  no `throw_ref: no guest handler found`, `raise builtin -> ud2`, or
  `[trap] ... re-raise` lines (all present in the old `/tmp/join-abi.log`).

## Remaining blocker (precise, bounded diagnosis)

Turn 1 stalls in a **host-side blocking wait**, not in a guest trap.  A bounded
diagnostic run with the same environment, sampled after 11 s, showed the main
thread in:

```
tid=<main> state=S wchan=futex_wait_queue
syscall=202 0x55d70e9c3e5c 0x89 0x0 <stack timespec> ...
```

`202` is `futex`, and `0x89` is `FUTEX_WAIT_BITSET_PRIVATE |
FUTEX_CLOCK_REALTIME`, the wait libstdc++ uses for
`std::condition_variable::wait_until`.  The only such waits in this host are
`imports.cpp` slot 5 (`__asyncjs__rbxFutexWaitMainJspi`, capped by a 30 s
`hard_stop`) and slot 325 (`_emscripten_thread_mailbox_await`, 5 ms).  Four
other threads were in state `R`.  `/proc/<pid>/mem` reads of the futex timeout
were rejected by the kernel (ptrace scope), so this is a strong inference from
the syscall/predicate, not a full stack trace; `gdb -p` could not unwind the
Cranelift frames (`Recursive internal problem`, no `.eh_frame_hdr`).

Scope note: this is no longer the worker-fault trap path described in
`host/ABI.md` §Remaining blockers 2 (that subagent owns worker faults).  The
stall is in a guest turn after the live-runtime exit — most likely the host's
bounded futex-wait import in the engine's startup path — and is outside the
slot-9/driver files owned by this task.  Per the bounded-investigation rule,
work stopped here rather than modifying read-only import implementations.
