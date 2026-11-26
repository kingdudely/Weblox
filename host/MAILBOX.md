# Mailbox notify (`_emscripten_notify_mailbox_postmessage`, slot 340)

Scope: bounded native-host investigation on branch `feat/cpp-mailbox`
(`/tmp/opencode/wt-cpp-mailbox`). No Chrome/Deno/game client was launched.
Reference implementations: `v2-wasmtime/src/imports/mailbox.rs`,
`v2-wasmtime/src/imports/futex.rs`, `v3-wasm2c/thread.c`, and the page glue
`docs/reference/research/WebClient.pretty.js` (`Cr`, `_o`, `Xa`).

## 1. What slot 340 actually received

A bounded `HOST_TRACE=1` join (`mailbox_join_final2.log`, verbatim):

```
[import] first call slot=340 env._emscripten_notify_mailbox_postmessage \
  args=0x5604acc79010,0xf1e890,0xf1e890,0x5604acb00000,0x5604acb00000
[rna-main] __main_argc_argv: trapped via signal 11
[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
```

`WB_TRACE_IMPORT` captures `rsi, rdx, rcx, r8, r9` (see `imports.h:67-80`).
For this import (`(void* vmctx, void* caller_vmctx, int32 target, int32 self)`):

| register | value | meaning |
| --- | --- | --- |
| `rdi` | `0x5604acc79010` | host vmctx |
| `rsi` | `0xf1e890` | caller vmctx (guest pthread pointer, not a host vmctx) |
| `rdx` | `0xf1e890` | `target` = main-thread `pthread_t` guest address |
| `rcx` | `0x5604acb00000` | `self` = **host linear-memory base**, not a 32-bit guest address |
| `r8`/`r9` | same as `rcx` | unused register residue (the import has only two i32 args) |

The linear-memory base in the same run is a high host pointer; `self` is
therefore not a valid guest offset. It must be treated as an opaque i32 and
must never be dereferenced through `wb::mem_*`. `target` (`0xf1e890`) *is* a
valid guest address (the main pthread struct).

The same shape was observed with the unmodified HEAD implementation
(`/tmp/opencode/mailbox_fault_orig.log`): target `0xf1e890`, self
`0x7f9368700000` (the memory base of that run). So the arguments and the fault
are not caused by this branch's diagnostics.

## 2. Why the old slot 340 cannot itself be the bad guest write

The pre-fix body (`host/imports.cpp:4554` on `feat/cpp-unwind`) only did:

```cpp
const auto box = wb::mailbox_for(static_cast<uint32_t>(a1));
{ std::lock_guard<std::mutex> lock(box->m); box->signaled = true; }
box->cv.notify_all();
```

`mailbox_for()` uses `target` as an `unordered_map` key; it never writes guest
memory. Thus the SIGSEGV was *not* an out-of-bounds guest write performed by
slot 340, nor a bad table-index callback: no guest re-entry happened there.

## 3. Where the fault actually is

* A temporary `HOST_TRACE`-only SIGSEGV probe (since removed: wrapping the
  host's trap handler measurably changed the failure point) logged the first
  fault captured in this investigation:

  ```
  [mbx!] SIGSEGV tid=... si_addr=0x8ad9a7 (host 0x8ad9a7)
         lib=/lib/x86_64-linux-gnu/libgcc_s.so.1+0x215d7
  ```

  `libgcc_s+0x215d7` is an internal unwind helper (just below
  `_Unwind_RaiseException` at `+0x22a60`). `0x8ad9a7` is a small, guest-like
  address near the EM_ASM key `0x8ada0e` from the preceding slot-13 call. The
  only host code that walks guest frames here is `trace_import_args()`'s
  `backtrace(frames, 10)` / `backtrace_symbols()` in `imports.cpp:760`. The
  linked object already reports `no .eh_frame_hdr table will be created`, so
  libgcc's CFI walk through Cranelift-generated frames is unreliable.

* In `mailbox_join_final2.log` the slot-340 argument line is printed and then
  the process traps **before** the new `[mbx ] notify ...` line in slot 340 is
  emitted. That places the fault between those two statements — i.e. in the
  first-call backtrace performed by `WB_TRACE_IMPORT(340)`, not in the mailbox
  helper.

* Without `HOST_TRACE` the backtrace is absent, but the join still ends with
  `trapped via signal 11` (`mailbox_join_notrace2.log`): the `[mbx] first
  notify accepted for thread 15853712` line shows slot 340 completed, then a
  later fault kills `__main_argc_argv`. So the backtrace is one trigger, not
  the only one.

* With gdb attached and signals passed through (`mailbox_gdb4.log`), the guest
  ran *past* slot 340 into `AppBridge setup + StartupController::globalInit`,
  gamepad registration, canvas sizing and input-callback imports before the
  run was stopped by the outer timeout. The fault is therefore
  timing/thread-scheduling sensitive, not a deterministic mailbox write.

* Starting the process under gdb or wrapping SIGSEGV handlers earlier in the
  boot changed the fault point (faults moved to slot 314 or slot 325), so all
  such diagnostic mechanisms were removed from the committed code. The
  committed diagnostics are passive `fprintf`s only.

## 4. Fix committed here (smallest honest change)

In `host/imports.cpp`, mailbox helpers and slot 340 only:

1. `trace_mailbox()` — bounded (`HOST_TRACE=1`, max 16 lines) log of `target`,
   `self`, both bounds checks, the host memory base/length, the pthread word at
   `target`, the mailbox pointer at `target+104`, the queue head/tail at
   `mbx+44`/`mbx+48`, and the ready word at `target+112`. It writes nothing.
2. Slot 340 bounds-checks `mem_ok(target, 116)` before using the pthread
   struct and `mem_ok(mbx, 52)` before reading queue fields. `self` is logged
   but never dereferenced (it can be a host pointer, see §1).
3. `mailbox_ring()` mirrors the reference notify: count the ring (bounded at
   1024), wake the guest's `Atomics.waitAsync(target, target)` through
   `wb::imports_futex_notify(target, 1)`, and signal the host condition
   variable waiter. No wasm re-entry from the notifying (possibly worker)
   thread; the guest's own drain is left to its safe points.
4. `imports.h` was **not** modified.

## 5. Verification (final tree)

```
make -j4                                            -> demo, clean compile
timeout 60s ./demo                                  -> exit 0, RESULT=OK,
                                                       ctors returned normally,
                                                       controlled errors=0
WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt \
WC_LOOP_MS=3000 timeout 90s ./demo --rna-probe      -> exit 0, trap=0,
                                                       connects=1 status=200
                                                       callbacks=1/0
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=... \
WC_LOOP_MS=3000 timeout 90s ./demo                  -> exit 0,
                                                       "trapped via signal 11",
                                                       connects=0,
                                                       no `turns=` lines
```

**Result:** the SIGSEGV is **not gone**. `main_loop_iteration`
(`_wasm_function_176`) never turns and no RNA import fires (`connects=0`). The
slot-340 arguments and ring delivery are now bounded and honest, but the
failure is downstream/deeper.

## 6. Remaining blockers (precise, stop here)

1. **Per-thread VMContext/TLS is missing.** `host/main.cpp::thread_entry()`
   copies the main vmctx into a `malloc`'d block and points every worker at the
   same `VMStoreContext` (`vmctx+8`), then overwrites the *shared*
   `stack_limit`. Workers never call `_emscripten_thread_init` /
   `_emscripten_tls_init`, so `pthread_self()` in a worker can observe the main
   thread's pthread struct (and, here, a host memory base). The unwinder's
   `last_wasm_exit_pc` / `last_wasm_exit_trampoline_fp` live in that shared
   store, so concurrent guest throws in workers race with the main thread.
   Fixing this requires edits to `host/main.cpp` / `host/vmctx.cpp`, which are
   outside this task's owned files.
2. **No mailbox drain hook.** Rust's `mailbox::pump_mailbox()` re-enters the
   guest's `_emscripten_check_mailbox` at each safe point and pumps HTTP/RNA.
   The C++ host has no such guest function pointer; `wb::g_guest_tick` only
   delivers one RNA completion. A counted ring is therefore accepted but not
   drained. Adding the hook needs a guest export lookup/registration in
   `main.cpp` or `vmctx.cpp`.
3. **Host backtrace over guest frames.** `trace_import_args()` uses glibc
   `backtrace()`; with Cranelift frames and no `.eh_frame_hdr` this faults in
   `libgcc_s` (§3). Remove/guard that backtrace (in the shared trace helper)
   before trusting `HOST_TRACE=1` join logs.
4. **Cross-thread `siglongjmp`.** `main.cpp::ExportTrapGuard::export_trap()`
   may run on a worker thread and `siglongjmp` into the main thread's
   `sigsetjmp` context. That is undefined behaviour and explains why timing
   changes (gdb, signal wrapping) move the fault.

This task was bounded to one serious slot-340 attempt; items 1-4 require
owned-file changes in other modules and are reported rather than attempted.
