# `HOST_TRACE` without a guest stack walk (`feat/cpp-trace`)

Scope: one bounded attempt on branch `feat/cpp-trace`
(`/tmp/opencode/wt-cpp-trace`). No Chrome/Deno/game client was launched.

## 1. The fault being removed

`host/MAILBOX.md` §3 records the previous `HOST_TRACE=1` join: the first-call
line for slot 340 was printed, then the process trapped in libgcc unwinding
(`si_addr=0x8ad9a7`, `libgcc_s.so.1+0x215d7`) before the mailbox handler's own
log line. The only code in the trace path that walks guest frames was
`trace_import_args()`'s `backtrace(frames, 10)` / `backtrace_symbols()` in
`host/imports.cpp`; the linked object reports
`error in /tmp/opencode/out.full.o(.eh_frame); no .eh_frame_hdr table will be created`.
Cranelift-generated frames cannot be reliably unwound by glibc/libgcc CFI.

## 2. Change (only `host/imports.cpp`, trace helper)

* Removed `#include <execinfo.h>` and both `backtrace()` / `backtrace_symbols()`
  calls. Nothing in the host trace path unwinds any stack now.
* The first-call line keeps slot, import name and the five captured argument
  registers (`rsi,rdx,rcx,r8,r9` from `WB_TRACE_IMPORT` in read-only
  `host/imports.h`) and appends one value: `ret=<guest return address>`.
* That value is **read directly off the live stack**, with no CFI walk:

  - the helper is marked `__attribute__((noinline))`, so it is a real call from
    the wasm import stub (verified in `objdump`: `call 459887` from
    `wasm_import_324`);
  - the build uses `-fno-omit-frame-pointer`, so `__builtin_frame_address(0)`
    is the helper's `rbp`; `[rbp]` is the saved caller (stub) `rbp`;
  - on x86-64 `[stub_rbp + 8]` is the return address pushed by the guest's
    `call` into the import stub. Two plain loads; libgcc never sees a guest
    frame. The compiler emits exactly `mov 0x0(%rbp),%rax; mov 0x8(%rax),%rdx`.

* Caveat: `host/gen_imports.py` is read-only for this task and still emits the
  old generator text for this region. Regenerating `imports.cpp` would put the
  backtrace back; that generator separately needs the same edit.

## 3. `ret=` is genuinely a Cranelift return address

From `HOST_TRACE=1 ./demo` (`host/trace_demo.log`, this tree):

```
[import] first call slot=324 env._emscripten_init_main_thread_js args=0x7fd99363c010,...,0 ret=0x55fd0e95b11a
[import] first call slot=325 env._emscripten_thread_mailbox_await args=...,0 ret=0x55fd0e95b1a7
```

Two unrelated argument registers in that same run are host function pointers:
`0x55fd0e921770` and `0x55fd0e922383` match `wasm_import_310_environ_get`
(`0x45e770`) and `wasm_import_313_clock_time_get` (`0x45f383`) in `demo` with
one page-aligned PIE load bias `0x55fd0e4c3000`. Subtracting that bias and the
linked code base (`_wasm_function_0` at `0x498000`) puts the two `ret=` values
at object offsets `0x11a` and `0x1a7`. Disassembly of
`/tmp/opencode/out.full.o` confirms both are immediately after indirect calls
through the import table:

```
     118: ff d0              call   *%rax      ; vmctx+0x28b0 -> import stub
     11a: 48 8b 74 24 68      mov    0x68(%rsp),%rsi   <- first ret=
...
     1a5: ff d0              call   *%rax
     1a7: 48 8b 7c 24 68      mov    0x68(%rsp),%rdi   <- second ret=
```

So `HOST_TRACE=1` still identifies the guest call site, just without symbol
names.

## 4. Verification (this tree)

```
make -j4                                  -> exit 0; only the pre-existing
                                             "no .eh_frame_hdr" linker warning
timeout 60s ./demo                        -> exit 0, "RESULT=OK",
                                             "__wasm_call_ctors ... returned normally",
                                             "controlled errors=0"
WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt WC_LOOP_MS=3000 \
  timeout 90s ./demo --rna-probe          -> exit 0, "probe ticks=589 trap=0
                                             connects=1 status=200 callbacks=1/0"
```

## 5. One bounded join with `HOST_TRACE=1`

Command (run once, from `host/`):

```
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt \
  WC_LOOP_MS=3000 timeout 90s ./demo
```

Result: exit 0 (the SIGSEGV is converted by the host's export trap guard),
199 lines of output (`host/join_trace.log`). Exact evidence:

```
173: [import] first call slot=340 env._emscripten_notify_mailbox_postmessage args=0x7f9a34828010,0xf1e890,0xf1e890,0x7f9934700000,0x7f9934700000 ret=0x55a42f3367aa
174: [mbx ] notify target=0xf1e890 self=0xf1e890 target_ok=1 self_ok=1 vmctx=0x7f9a34828010 caller=0x7f9a34828010 base=0x7f9934700000 len=4294967296 word=0xf1e890 mbx=0xf60fd8 q(44,48)=0/0x1 ready=0 self_word=0xf1e890
175: [import] [mbx] first notify accepted for thread 15853712
176: [log ] [WASM-TIMING] AppBridge setup + StartupController::globalInit: +3585.1ms from preInit, phase took 1024.3ms
177: [log ] [WASM-TIMING] OnAppStarted: +3585.2ms from preInit, phase took 0.0ms
...
194: [import] first call slot=21 env.emscripten_set_mousedown_callback_on_thread args=0x7f9a34828010,0xe2b50,0,0x1,0x11 ret=0x55a41f3cce5f
195: [rna-main] __main_argc_argv: trapped via signal 11
196: [rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
197: [wasm-host] import calls=0 controlled errors=0
199: [wasm-host] RESULT=OK
```

### Answers

1. **Does the slot-340 fault disappear? Yes.** The old sequence was
   "first-call line → SIGSEGV in libgcc → no `[mbx ] notify` line" (MAILBOX.md
   §3). Here the traced line is followed by the mailbox handler's own lines
   (174-175) and then by `AppBridge setup + StartupController::globalInit`,
   `OnAppStarted`, gamepad imports (slots 70/72/73/74), canvas imports
   (slots 15-19), `install_pointer_source_tracking` (slot 20) and the first
   mouse callback import (slot 21). No libgcc unwind fault occurs.

2. **Does the join still SIGSEGV later? Yes.** `__main_argc_argv` is reported
   as "trapped via signal 11" at line 195, immediately after line 194. Last
   observed guest activity is the first call of import slot 21
   (`env.emscripten_set_mousedown_callback_on_thread`); that stub is a pure
   generated stub (returns 0, logs nothing else), so the fault is in guest code
   after it returns, or on a worker thread racing it. No slot 22 trace line
   appears. (Worker `pthread_start` SIGSEGVs also appear earlier at lines 59
   and 163-171; they predate this change.)

3. **Does `main_loop_iteration` run? No.** There are no `turns=` or
   `loop turns=` lines; the loop is gated on `main_sig == 0` in
   `host/main.cpp`, which never holds.

4. **Do RNA imports fire? No.** `connects=0 connect_errors=0 callbacks=0/0
   status=0 error=`; the RNA connect path is not reached in this join path
   (the separate `--rna-probe` check above does connect, `connects=1`).

## 6. Stop condition

Per the bounded task, no second join and no changes to other modules were
attempted. Removing the guest stack walk fixes the slot-340 trace fault, but it
does **not** explain the remaining SIGSEGV at line 195 after slot 21. The
previously reported blockers (per-thread TLS/vmctx, missing mailbox drain hook,
cross-thread `siglongjmp`) in `host/MAILBOX.md` §6 still stand.
