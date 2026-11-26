# Guest mailbox drain (`pump_mailbox` / `_emscripten_check_mailbox`)

Branch `feat/cpp-drain` (`/tmp/opencode/wt-cpp-drain`). No Chrome/Deno/game
client was launched; this is a bounded native-host change with one join
attempt. Reference: `v2-wasmtime/src/imports/mailbox.rs` and
`v2-wasmtime/src/main.rs`.

## 1. The guest drain entry

`host/table.h` contains only element-segment entries — the mailbox drain is an
**export**, not a table element. Parsing the export section of
`/tmp/opencode/WebClient.raknet.wasm` (85 exports) gives:

```
EXPORT _emscripten_check_mailbox kind=0 index=150299
```

Function index 150299 minus the first defined index (341, see `main.cpp`'s
export comments) is `_wasm_function_149958`. The function section gives it
type 15, `() -> ()`, i.e. the same raw ABI as `main_loop_iteration`:
`void _wasm_function_149958(void* callee_vmctx, void* caller_vmctx)`. The
symbol exists in `/tmp/opencode/out.full.o`:

```
00000000108da020 T _wasm_function_149958
```

Rust calls this export by name (`inst.get_export("_emscripten_check_mailbox")`);
the AOT object has no names, so the C++ host calls the numbered symbol
directly, with `(vmctx, vmctx)` just like the other exports in `main.cpp`.

## 2. What changed

* **`host/imports.cpp`** (mailbox helpers / slot 340 region only):
  * `Mailbox` now keeps a **counted per-target ring** (bounded at 1024),
    mirroring `mailbox.rs`'s `RINGS`; `mailbox_ring()` also keeps the global
    pending count used for cheap safe-point checks.
  * `wb::mailbox_drain(vmctx)` is the C++ `pump_mailbox`: it runs only on the
    thread that owns the main loop (thread id recorded in slot 324 and on the
    first drain), re-enters no wasm at all on a worker, writes the pthread
    "ready" word at `target+112` only after `mem_ok(target, 116)`, collects at
    most 64 targets, consumes one bounded ring per target, and calls
    `_wasm_function_149958(vmctx, vmctx)` once. `in_drain` blocks recursion.
  * The guest call is wrapped in a **thread-local sigsetjmp/siglongjmp**
    handler: a trapping drain calls `host::note_error(...)` (controlled error,
    `HostState.error_count`) instead of unwinding past the caller. A signal on
    the RNA pumper thread can never jump into the loop thread.
  * Slots 4 (`emscripten_get_now`) and 5 (futex wait) call
    `wb::mailbox_drain_if_pending()` **after** the existing `g_guest_tick`
    completion pump. This is the Rust `mailbox::pump_now` safe point: it is a
    no-op unless a ring is actually pending.
  * Slot 340 still only records a ring and wakes the futex/host waiter. It
    never dereferences `self` (which is a host memory base on this ABI) and
    never re-enters wasm. No change to `imports.h`.

* **`host/loop.cpp`** (new): hooks the existing main-loop turn. This branch
  has no separate `run_loop()` driver — `main.cpp` owns the bounded deadline
  loop and calls `_wasm_function_176` inline. Rather than edit that read-only
  file, the Makefile links with `-Wl,--wrap=_wasm_function_176`, so each turn
  goes through `__wrap__wasm_function_176`, which (1) pumps one RNA
  completion, (2) drains the mailboxes via `wb::mailbox_drain`, (3) calls the
  real iteration under the driver's existing guard, and (4) drains again for
  rings produced by the turn. All re-entry is on the loop thread; traps in
  the hook itself become controlled errors and do not stop the turn. Bounded
  `[drain]` traces are emitted every 200 turns and at each actual drain.

* **`host/Makefile`**: compile/link `loop.cpp` and add `WRAPFLAGS`.

## 3. Verification (all bounded; exactly one join attempt)

Build: `cd host && make -j4` → exit 0. Only the pre-existing
`no .eh_frame_hdr table will be created` linker note (see `MAILBOX.md`) and
no new compiler warnings. Logs: `/tmp/opencode/wt-drain-build.log`.

### `timeout 60s ./demo`

Exit 0; `__wasm_call_ctors` returned normally, `controlled errors=0`,
`RESULT=OK`. No regression (`/tmp/opencode/wt-drain-demo.log`).

### `timeout 90s ./demo --rna-probe`

`WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt WC_LOOP_MS=3000`, exit 0:

```
[rna ] -> rbx_wasm_rna_on_connect_success id=1
[rna-main] probe ticks=590 trap=0 connects=1 status=200 callbacks=1/0 ring=0/0
```

The RNA completion path still delivers a real guest callback with zero traps
(`/tmp/opencode/wt-drain-probe.log`). The probe does not take a
`main_loop_iteration` turn, so it does not exercise the wrap hook.

### One bounded join attempt

`HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt WC_LOOP_MS=3000
timeout 90s ./demo` → exit 0. Log: `/tmp/opencode/wt-drain-join.log`.

```
[import] first call slot=340 env._emscripten_notify_mailbox_postmessage ...
[mbx ] notify target=0xf1e890 self=0xf1e890 target_ok=1 self_ok=1 ... q(44,48)=0/0x1 ready=0
[import] [mbx] first notify accepted for thread 15853712
...
[drain] main=0xf1e890 readied=1 pending=0 guest_calls=1 traps=0
...
[rna-main] __main_argc_argv: trapped via signal 11
[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
[wasm-host] import calls=0 controlled errors=0
```

* **Mailbox delivery errors:** none. The ring was accepted, the guest
  `_emscripten_check_mailbox` was re-entered once (`guest_calls=1`), the ready
  word was written, `pending` went to 0, `traps=0`, and controlled errors
  stayed at 0. This is the behaviour that was missing in `MAILBOX.md` §6.2.
* **`main_loop_iteration` turns:** 0. `__main_argc_argv` still traps with
  SIGSEGV later in bootstrap (after gamepad/canvas imports at slot 21), so
  the driver never enters its loop. Consequently the new wrap hook was
  **installed and linked but not executed** in this run; the drain observed
  above came from the slot-4/5 safe point.
* **RNA imports in the join:** `connects=0`, `callbacks=0/0` — unchanged and
  for the same reason (bootstrap never returns). The probe above proves the
  RNA/completion side independently.

## 4. Precise remaining blocker (stop here)

The join failure is no longer at the slot-340 notify: with the drain in place
the guest runs from slot 340 through `AppBridge setup + StartupController::
globalInit`, gamepad registration, canvas sizing and pointer imports before
`__main_argc_argv` traps via signal 11. Per `MAILBOX.md` §6 the remaining
causes are outside this task's owned files: per-thread VMContext/TLS, host
`backtrace()` over Cranelift frames, and cross-thread `siglongjmp` from
`main.cpp`'s `ExportTrapGuard`. This task was bounded to one join attempt;
no further join was run and no read-only file was changed.
