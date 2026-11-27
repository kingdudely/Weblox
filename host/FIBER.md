# Native stack switching (fibres) for JSPI-style imports

Branch `perf-fiber` (`/tmp/opencode/wt-perf-fiber`). One bounded investigation;
no Chrome/Deno/game client was launched.

## 1. What was implemented

* **`host/fiber.h` / `host/fiber.cpp` (new).** A thread-local `ucontext_t`
  fibre. The driver allocates a 32 MiB stack (`mmap` + `PROT_NONE` guard page),
  `makecontext()`s a trampoline, and `swapcontext()`s into it. The guest turn
  runs on that stack; `fiber::yield()` saves it and switches back to the
  driver's context; `fiber::resume()` restores the *same* context, so the wasm
  call continues at the instruction after the yield. `uc_link` returns to the
  driver when the turn finishes normally. All state is `thread_local` and
  `yield()` additionally checks that the caller's stack pointer lies inside
  the fibre stack, so a re-entrant RNA callback run on the driver stack can
  never switch the wrong context. No new dependencies (no Boost.Context).

* **`host/imports.cpp`, slot 5 only.** Before the driver exists (ctors /
  smoke-test paths) the previous bounded 2 ms non-blocking poll from
  `feat/cpp-jspi` is kept. With the fibre driver active, the first wait on a
  changed-or-unchanged word yields instead of waiting: the wasm call is
  suspended, not polled. On resume the guest word is re-read; the import
  returns `1` ("not-equal") if it changed, `0` ("ok") if `waiter_for(addr)`
  saw a notify or the driver's completion pump set `woken`, otherwise `2`
  ("timed-out"). The trace line now carries `fibered=0|1`.

* **`host/Makefile` only (no edits to read-only `host/loop.cpp` /
  `host/main.cpp`).** `-Wl,--wrap=<mangled weblox::run_loop>` redirects the
  turn driver to `fiber.cpp`. The wrapper keeps the driver's observable
  contract (deadline, trace lines, periodic RNA report, 2 ms turn cadence)
  but, when a turn is suspended, pumps `rna_guest_tick()` + mailbox drain and
  resumes the existing fibre instead of re-entering `main_loop_iteration`.
  A parked turn is allowed to finish past `WC_LOOP_MS` up to a hard +10 s cap,
  because asset fetches inside turn 1 routinely consume the 3 s window before
  the first yield. The existing `-Wl,--wrap=_wasm_function_176` hook in
  `loop.cpp` is unchanged and still brackets every fresh turn.

## 2. Verification

`make -j4` — exit 0. Only the pre-existing
`/usr/bin/ld: error in /tmp/opencode/out.full.o(.eh_frame); no .eh_frame_hdr
table will be created` warning. `nm demo` shows both
`__wrap__ZN6weblox8run_loopE...` and loop.cpp's `weblox::run_loop`.

`timeout 60s ./demo` — exit 0, `[wasm-host] RESULT=OK`,
`import calls=0 controlled errors=0`, `_wasm_call_ctors ... returned normally`.

`timeout 90s ./demo --rna-probe` — exit 3, the expected pre-network failure
`[rna-main] join attempt failed: empty .ROBLOSECURITY cookie` (no cookie was
supplied). Same as the baseline branch.

Bounded join (run with the final wake semantics; log
`/tmp/opencode/fiber-join2.log`, 7003 lines, exit 0 in ~13 s):

```
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt \
  WC_LOOP_MS=3000 timeout 90s ./demo
```

Exact lines:

```
260: [fiber] run_loop entered (wrapped iteration + fibre)
261: [fiber] turn 1 begin
262: [drain] main=0xf1e890 readied=1 pending=0 guest_calls=2 traps=0
271: [import] [futex] main-thread waitAsync suspended on native fibre
457: [futex] call=1000 addr=0x15a1b8c exp=0 a3=inf cur=0 fibered=1
2276:[fiber] resumes=1000 suspended=1
...
6466:[futex] call=4000 addr=0x15a1b8c exp=0 a3=inf cur=0 fibered=1
6991:[rna-main] loop turns=0 iter_trap=0
6992:[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
```

Answers to the verification questions:

* **Does turn 1 complete? No.** There is no `[fiber] turn 1 end`, no
  `turn 2 begin`, and `turns=0` in the final report. The guest is still inside
  the same `main_loop_iteration` call when the +10 s hard cap expires.
* **Do turns advance? No** — for the same reason: `turns_out` only increments
  when the wrapped turn returns, and that never happens.
* **Does `connect_js` (slot 96) fire? No.** No `first call slot=96` line, no
  RNA CONNECT, `connects=0`, `callbacks=0/0`, `status=0`. There is no CONNECT
  status or callback count to report. (`status=2` on the earlier
  `[rna-main] route:` line is the gamejoin route fetch, not RNA CONNECT.)

Two diagnostic joins were also made while isolating the blocker, both bounded
by `timeout 90s`:

* First attempt (`fiber-join.log`): the driver exited at the original
  `WC_LOOP_MS=3000` deadline as soon as the guest yielded, because the asset
  fetches in turn 1 had already used the window. This produced `loop turns=0`
  but no resume at all. Fixed by treating the deadline as a turn boundary and
  allowing a suspended turn to be resumed up to +10 s.
* Synthetic-wake experiment (`fiber-join3.log`): forcing `woken=true` on every
  resume made slot 5 return `0` instead of `2`. The guest re-armed the same
  wait anyway (7000+ calls, `addr=0x15a1b8c exp=0x1 cur=0x1 a3=inf
  fibered=1`), still with no slot 96. That code was **reverted**: a spurious
  "ok" does not help because the guest loops until the *word* changes.

## 3. What works, and the precise remaining blocker

The fibre mechanism itself works: no unwind/CFI failure was observed with
Cranelift frames. `swapcontext()`/`swapcontext()` round-trips 7000+ times
without a trap on the loop thread, the guest continues in the same frame after
each resume, the `[futex] call=N` sequence advances monotonically, and the
baseline worker-thread traps (`[pthr] worker ... start=417 trapped via signal
4/11`, contained by their own guards) are unchanged and are not on the loop
thread.

The blocker is **not** the native wait or the return code: it is that nobody
writes the guest futex word `0x15a1b8c`. In every join the same word is waited
on with an unbounded timeout (`a3=inf`) and `cur == expected` at every
resume; neither `memory.atomic.notify` nor the mailbox path ever targets it
(`mailbox_ring()` notifies the pthread address `0xf1e890`, and
`mailbox_drain()` writes `pthread+112`). Worker threads that could post the
real notification keep trapping (signal 4/11) before they can do so. Host
completion pumping cannot help because slot 96 (`connect_js`) has not run yet,
so there is no completion to pump.

## 4. Limits

* Fabricating a guest-visible wake (writing `0x15a1b8c`, or returning a
  synthetic `0`) does not make the guest progress and is not done here; both
  were measured above. A real fix needs the missing notifier (worker-thread
  startup/mailbox post) or a wasm-level async/JSPI completion hook.
* `ucontext_t` is legacy POSIX (not C++ standard) but available in glibc with
  no extra dependency. The driver restores the signal mask through
  `swapcontext`; `fiber::yield()` must never be called from a stack other than
  the fibre's (enforced by the stack-range check).
* A trap that `siglongjmp`s out of `ExportTrapGuard` while a turn is suspended
  would target a non-running fibre frame; driver-side hook work therefore
  uses its own thread-local signal guard, and host code in the driver must
  not fault while a turn is parked.
* The hard +10 s extension keeps a never-waking guest bounded under
  `timeout 90s`; it is not a guarantee that turn 1 finishes.
* `host/imports.h`, `host/main.cpp`, `host/loop.cpp`, the vmctx/unwind/pthread/
  route/RNA files, generators and generated files were left untouched.
