# Rust/Wasmtime host: join attempt on the stock wasm (2026-11-26)

Scope: `v2-wasmtime/` on branch `rust-join`, worktree `/tmp/opencode/wt-rust-join`.
No wasm patching, no guest-pointer handoff, no `WC_RAKNET`. The module is the
stock `/home/john/rr/WebClient.wasm` (102,268,042 B); the cookie is
`/home/john/mizg.txt`; place 189707.

## Commands

Cold build (vendored Wasmtime 50 crates; first build took 16 m 49 s):

```
cd /tmp/opencode/wt-rust-join/v2-wasmtime
cargo build --release
```

The 102 MB module must be precompiled once (321 s; cached in `data/cache/`):

```
WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt WC_LOOP_MS=0 \
  timeout 900 ./target/release/alcohol 189707
```

Join run (180 s, stock module, WebTransport/RbxTransport route):

```
cd /tmp/opencode/wt-rust-join/v2-wasmtime
WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt WC_LOOP_MS=60000 \
  timeout 180 ./target/release/alcohol 189707 > /tmp/opencode/rust-join-verify.log 2>&1
```

Diagnostic variants used (all env-only, no rebuild):

```
WC_HTTP=1 WC_TRACE=1 WC_FIB=1 ...   # baseline / fixed comparison
WC_STALL_MS=100000 WC_MBX=1 WC_FUTEX=1 WC_ASYNC=1 WC_GLES=1 WC_JOINDUMP=1
```

## Root cause found and fixed

Baseline log: `/tmp/opencode/rust-join-baseline.log` (and `rj-diag1.log`).
Every guest fibre creation failed immediately:

```
[fib ] create tok=0x518615f stack=51c61e0..5186280 entry=446
[fib ] tok=0x518615f entry=446 ended: store configuration requires that `*_async` functions are used instead
```

Wasmtime 50 sets `async_required` on a store as soon as **one** `func_wrap_async`
import is registered (`HostFunc::into_func` → `set_async_required(Asyncness::Yes)`).
`Store::validate_sync_call` then rejects *all* subsequent `Func::call` /
`TypedFunc::call` on that store. The four remaining synchronous calls were the
guest-stack setters in `src/fibre.rs`:

* `Rt::restore()` — `set_limits.call`, `set_sp.call`
* `run_entry()`   — `set_limits.call`, `set_sp.call`

The module's JSPI fibre protocol (`__asyncjs__create_suspended_fiber_js`,
`__asyncjs__fiber_transfer_js`) starts every guest fibre through `run_entry`,
so all of them died before entry. Worker callbacks, network completions and the
engine's deferred work never ran. In the baseline this showed up as a busy
turn loop (~9,500 empty `main_loop_iteration` turns in 13 s), no join POST,
and repeated watchdog restarts (`[boot] stalled, restarting`).

**Fix** (`v2-wasmtime/src/fibre.rs`, the only code change): replace those four
`call` invocations with `call_async(...).await`. Both setters are plain wasm
globals writes, so they complete inline; no extra yield was introduced between
the two stack-pointer writes. This keeps Wasmtime's own async machinery as the
suspension mechanism instead of hand-rolling a stack switch.

## Verification after the fix

Logs: `/tmp/opencode/rust-join-fixed1.log` (instrumented),
`/tmp/opencode/rust-join-verify.log` (exact prescribed command, no extra env).

Now reached, on the stock wasm, with no `WC_RAKNET`:

```
[log ] [LIFECYCLE] joinGameSucceeded
[join] 200 {"jobId":...,"status":2,...
[join] route 97a9277a81ca6c61d279bd9d9516972f-0a2503d0-c535.80740222.qdmux.roblox.com
[log ] ... [DFLog::NetworkClient] Transport selection: useRbxTransportEnabled=true,
       rbxTransportHasValidPort=true, rbxTransportHasValidAddress=true,
       rbxTransportHasValidPubKey=true, canUseRbxTransport=true,
       selectedTransport=RbxTransport, rccPort=50485, DatacenterId=26184
[warn] ... [FLog::Network] UDMUX Address = 128.116.2.33, Port = 50485 |
       RCC Server Address = 10.37.3.208, Port = 50485
```

`patch_join()` derived a `gameFqdn` from the native-UA join reply (verified with
`WC_JOINDUMP=1`: `NetStackPort`, `NetStackTokenValue`, `NetStackConfig` with
`pubKey`/`certHashes` all present). Turn loop now does ~51,000 real turns in
60 s instead of spinning with no guests.

Still missing (verification log, counts all zero):

| signal | count |
|---|---|
| `[rna ] connect_js` | 0 |
| `[rna ] conn=` / `[rna ] connected` | 0 |
| `CONNECT 200` | 0 |
| `Assigned peer id` | 0 |
| `#players` | 0 |

Also zero: any `rbx_wasm_rna_*` runtime import call (only the embind bindings
for the callbacks are registered). No transport-selection rejection
(`WC_BT_LOG=1`), no guest crash report, no `unwind` other than the expected
`emscripten_exit_with_live_runtime`, no futex spin (`WC_FUTEX=1`), no queued
`emscripten_async_call` (`WC_ASYNC=1`).

Exact last engine line in the verification log:

```
[warn] [6] 2026-12-27T02:26:15.607Z,36.607079,2ce8980,6 [DFLog::HttpTrace] HttpRequest(#12 0xd0923c8) roundtrips: 1 delayMs: 0
```

Last host event before the loop ends:

```
[wasm] 51440 turns; last event http POST https://ephemeralcounters.api.roblox.com/v1.1/Counters/BatchIncrement?apiKey=...
```

`WC_GLES=1` (force the GLES device path) also reached transport selection but
not `connect_js`; so the WebGPU surface warning
(`Mode 4 failed: WebGPU window surface capabilities are unavailable`) is not
the gate. `player-hydration-service` answers 415 in both this host and the
older successful logs, so it is not a discriminator either.

## Remaining blocker

The engine accepts RbxTransport and logs the UDMUX endpoint, but never reaches
the wasm call site of `rbx_wasm_rna_connect_js`. This is now **after** the JSPI
blocker (fibres demonstrably run: ~51k turns, join POST, transport selection).
It is not a settings/flag or join-reply problem: `selectedTransport=RbxTransport`
with all four eligibility flags true, and the crafted `gameFqdn` is present in
the delivered joinScript. It is not the transport backend: earlier v2 runs
(`/tmp/opencode/h13.log`, `p11.log`) show the same backend completing
`[rna ] connected id=1 maxdgram=1149` once the engine calls the import, and the
Deno client (`docs/CHROME-ORACLE.md`, `client/page/draftwt.ts`) joins with
CONNECT 200 and `#players = 21`.

Next step would be to locate the wasm call site of `rbx_wasm_rna_connect_js`
(function indices 843/844/943/954/154240 appear in the benign destroyed-fibre
traces) and find which deferred continuation after transport selection is not
being resumed. That was out of scope for this bounded attempt.

## Files touched

* `v2-wasmtime/src/fibre.rs` — four `call` → `call_async(...).await` changes.
* `v2-wasmtime/docs/RUST-JOIN.md` — this record.

No changes to `wt.rs`, the wasm binary, settings flags, or guest memory hooks.
