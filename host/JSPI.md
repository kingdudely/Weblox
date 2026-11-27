# Slot 5 (`__asyncjs__rbxFutexWaitMainJspi`): non-blocking main-thread polling

## What changed and why

`host/imports.cpp`, **slot 5 body only**. The pre-change implementation
repeatedly called `cv.wait_until` with a 2 ms slice until either the guest word
changed or a 30 s `hard_stop` elapsed. That is a real block of the one thread
that drives `main_loop_iteration`, so after the live-runtime exit (`feat/cpp-liveruntime`)
turn 1 could begin but never finish: the sampled main thread sat in
`futex(FUTEX_WAIT_BITSET_PRIVATE|CLOCK_REALTIME, ...)` (syscall 202, op `0x89`)
for the whole join window.

In a browser the JS glue is:

```js
function En(e, r, t) {
  return O.handleAsync(async () => {
    const n = t === 1 / 0 ? void 0 : t;
    const a = Atomics.waitAsync(m, e >> 2, r, n);
    if (a.async) { const i = await a.value; return i === "ok" ? 0 : i === "not-equal" ? 1 : 2; }
    return a.value === "ok" ? 0 : a.value === "not-equal" ? 1 : 2;
  });
}
```

JSPI *suspends* the wasm call; the page event loop keeps running and RNA
completions keep arriving. This host has no fibre or promise pump, so the
closest honest emulation is a single bounded slice, never a loop:

* GuestTick and mailbox drain still run at entry (unchanged).
* `addr` out of range or `word != expected` returns `1` ("not-equal")
  immediately.
* If `word == expected`, create/consult the futex waiter, do **one**
  `cv.wait_until` capped at **2 ms** (the `a3` timeout is explicitly ignored —
  no native wait may outlive the slice), consume a notify, then return `2`
  ("timed-out") unless a notify actually landed (`0`, "ok").

Consequence, by design: this emulates "the main thread never blocks", but it
cannot emulate suspension. It produces far more `2` returns than a JSPI host,
and for `a3 = inf` the guest re-arms the wait from its own loop instead of
being parked in the browser's promise. See the measured effect below. The
bounded `HOST_TRACE` counter (`[futex] call=N ...`) was added to make that
retry loop visible; `note_once("[futex] main-thread waitAsync polled ...")`
records the first use. No other import implementation, header, generated file,
or driver file was touched.

## Verification (branch `feat/cpp-jspi`, `/tmp/opencode/wt-cpp-jspi/host`)

### 1. `make -j4`

Exit 0. Regenerated `table.cpp`, `runtime_data.cpp`, `abi_stubs.S` and rebuilt
`demo`. The only diagnostic is the pre-existing
`/usr/bin/ld: error in /tmp/opencode/out.full.o(.eh_frame); no .eh_frame_hdr
table will be created`.

### 2. `timeout 60s ./demo`

Exit 0; ends `[wasm-host] RESULT=OK` (`/tmp/jspi-demo.log`).

### 3. `timeout 90s ./demo --rna-probe`

Exit 3, the expected pre-network failure (no cookie given):
`[rna-main] join attempt failed: empty .ROBLOSECURITY cookie`
(`/tmp/jspi-probe.log`). Same as the baseline branch.

### 4. One bounded join

```
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt WC_LOOP_MS=3000 timeout 90s ./demo
```

Exit 124 (`timeout` killed it at 90 s). Full log: `/tmp/jspi-join.log`. Exact
lines (line numbers from that file):

```
45:  [rna-main] prepare ok=1 trap=0 meta=data/meta.json content=data/content
46:  [rna-main] __main_argc_argv argc=1 argv=27604456 (argv_trap=0)
58:  [import] first call slot=5 env.__asyncjs__rbxFutexWaitMainJspi ...
59:  [import] [futex] main-thread waitAsync polled (no JSPI fibre)
241: [live] slot 9 vmctx=0x7f058fd0d010 caller=0x7f058fd0d010 main-return, keep runtime
242: [rna-main] __main_argc_argv: live runtime (slot 9): main returned, runtime alive
243: [loop] run_loop entered (wrapped iteration)
244: [loop] turn 1 begin
245: [drain] main=0xf1e890 readied=1 pending=0 guest_calls=2 traps=0
252: [import] first call slot=3 env.js_fetch_file ...
253: [import] [fetch] first js_fetch_file: .../InExperiencePatchConfig.json
254: [import] [fetch] first js_fetch_file: .../InExperience.rbxm
255: [warn] ... Hello World! CLI-232246
256: [import] first call slot=53 env.emscripten_asm_const_int_sync_on_main_thread ...
```

Answers to the verification questions:

* **Do `[loop] turn N` lines advance?** No. `[loop] turn 1 begin` is printed
  once (line 244); there is no `turn 1 end`, no `turn 2 begin`, no
  `[rna-main] loop turns=... iter_trap=...` line and no `[rna-main] final:`
  line before the 90 s kill. With `WC_LOOP_MS=3000` the driver should have
  finished after 3 s of turns, so a single guest call inside turn 1 consumed
  the entire window.
* **Does `connect_js` (slot 96) fire?** No. No `first call slot=96
  env.rbx_wasm_rna_connect_js` line appears; no progress report (printed every
  100 turns) and no final RNA stats were reached. Consequently there is no RNA
  CONNECT status or callback count to report. The `status=2` on the earlier
  `[rna-main] route:` line is the gamejoin route fetch, not an RNA CONNECT.
* **Remaining traps?** No new fatal trap on the loop thread after slot 9.
  Pre-existing worker faults (`[pthr] worker ptr=... start=417 trapped via
  signal 4`, e.g. lines 66, 95, 97, 168, 172) still occur, but those are on
  worker threads, not the loop thread, and no `[trap]`/throw_ref lines follow
  slot 9.

## Remaining blocker: precise, bounded diagnosis

The blocking wait is gone, but turn 1 still does not finish. A second,
instrumented 25 s run with the same environment (`/tmp/jspi-trace5.log`) added
a bounded per-call counter to slot 5 and showed what replaces the block:

```
253: [live] slot 9 ... main-return, keep runtime
254: [rna-main] __main_argc_argv: live runtime (slot 9): main returned, runtime alive
256: [loop] turn 1 begin
269: [futex] call=1000 addr=0x15a1b8c exp=0x1 a3=inf cur=0x1
...
277: [futex] call=9000 addr=0x15a1b8c exp=0x1 a3=inf cur=0x1
```

All 9000 calls in turn 1 target the **same** guest word `0x15a1b8c`, with
`expected = 1`, `cur = 1` and an **unbounded** timeout (`a3 = inf`). Every one
returns `2` after its single 2 ms slice (the first ~12 calls, during ctors,
used `a3 ≈ 999…974 ms` and also returned 2). The guest re-arms the same wait
immediately, so `main_loop_iteration` never returns and the driver cannot take
another turn or deliver the completion that would change that word. Sampling
the live process during this phase still shows the main thread in
`futex_wait_queue` (syscall 202, op `0x89`) — now because of the repeated
2 ms slices, not because of a 30 s wait.

This is a **guest retry loop on the same import**, not a different blocking
import: slot 325 (`_emscripten_thread_mailbox_await`) still defers immediately
on the main thread, and the only other host waits are bounded 2 ms/5 ms sleeps
in the driver and bridge threads. Per the bounded-investigation rule, work
stopped here. Breaking the loop would require either a real JSPI fibre (to
suspend instead of returning `timed-out`) or fabricating a guest-visible wake,
neither of which is within this change's honest-emulation contract.

`host/imports.h`, `host/main.cpp`, `host/loop.cpp`, the vmctx/unwind/bridge
files and all generated files were left untouched.
