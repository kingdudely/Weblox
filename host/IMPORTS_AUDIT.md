# Import audit: which zero-returning slots does the engine actually call?

Scope: one bounded audit pass on branch `feat/cpp-imports`
(`/tmp/opencode/wt-cpp-imports`). This document records **evidence**, not
speculation: every slot listed in §2 was observed in a `HOST_TRACE=1` native
join log. Companion helper: `host/import_trace.py` with the `host/import_trace.sh`
wrapper. No Chrome/Deno/game client was launched.

## 0. Bottom line

1. **No zero-returning import slot is hit before the current join failure.**
   Across all available `HOST_TRACE=1` logs, 32 distinct import slots are
   called; **all 32 already have real implementations**. The observed stub-hit
   count is **0**.
2. The join attempt does not reach stub-dependent code. It gets through
   `__wasm_call_ctors` and `prepare()`, enters `initialize()`, and then traps
   on the **`throw_ref` builtin** (`[trace] throw_ref exnref=0x8 -> not
   caught`, "exception propagation not implemented") in `__main_argc_argv`.
   That is a *builtin*, not one of the 341 imports. Fixing more stubs alone
   will not change today's join outcome; the exn/unwind path is the blocker.
3. Nine stubs were nevertheless promoted to honest, reference-backed
   implementations (§4) because they are the first getters/host no-ops a
   successful boot needs, and they now stop faking a return where a real value
   was available. Counts went from **74 real / 267 zero-stubs** to
   **83 real / 258 stubs**; of the 258, 12 are RNA slots bridged at runtime by
   `rna_bridge.cpp`, leaving **246 genuinely zero-returning** slots.
4. Slots 86–97 (RNA) are functionally unchanged (their hand-written
   forwarders are preserved verbatim on regeneration), the seven embind
   setter bodies were not touched, and no generated declaration was
   hand-edited (see §6).

## 1. Evidence and reproduction

Logs parsed (all native C++ host, `HOST_TRACE=1`):

| Log | Origin | Result |
| --- | --- | --- |
| `/tmp/opencode/wt-cpp-bootstrap/host/join_run.log` | historic join attempt (49 lines) | 32 first-call lines; `throw_ref` trap at `initialize()` |
| `/tmp/opencode/wt-prepare-join.log` | historic prepare/join run (108 lines) | same 32 slots; same trap |
| `/tmp/opencode/host_table_trace.log` | early ctors-only trace (32 lines) | subset of the same 32 slots |
| `host/imports-join-trace.log` | fresh bounded run on this branch, before the fixes | 32 first-call lines, 0 stubs |
| `host/imports-join-trace-after.log` | bounded run after the first regeneration | 32 first-call lines, 0 stubs |
| `host/imports-join-trace-final.log` | final bounded run, after RNA forwarders were preserved and the demo rebuilt | 32 first-call lines, 0 stubs |

Fresh-run command (one bounded `timeout 90s` after `make`, from `host/`):

```sh
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/king.txt \
WC_META=../v2-wasmtime/data/meta.json \
WC_CONTENT=/tmp/opencode/wt-cpp-imports/v2-wasmtime/data/content \
  timeout 90s ./demo > imports-join-trace.log 2>&1
./import_trace.sh imports-join-trace.log
```

`wb::trace_import_args()` and `wb::import_log_first()` both call
`g_seen[slot].exchange(true)` (`imports.cpp:729-763`), so each slot's *first*
call is logged exactly once per process; a stub hit cannot hide behind an
earlier unlogged call. The helper cross-checks log names against the generated
declarations and marks RNA slots as `bridge` (they are overwritten at runtime).

```sh
./import_trace.sh --all-stubs imports-join-trace-final.log   # hits + full stub catalog
./import_trace.sh --stub-catalog                             # 246 stubs + 12 bridged
```

## 2. All import slots hit during bootstrap, ranked by first call

Signatures are the wasm signatures from `imports.h`. "Reference" is what the
working Rust host (`v2-wasmtime/src/imports/*.rs`) / wasm2c host
(`v3-wasm2c/*.c`) does; "C++" is the implementation in `host/imports.cpp`.
Phases: **C** = `__wasm_call_ctors`, **P** = `prepare()`, **I** = `initialize()`.

| # | Slot | Import (signature) | Phase | Reference behavior / C++ implementation |
| --- | --- | --- | --- | --- |
| 1 | 324 | `env._emscripten_init_main_thread_js(i32)->void` | C | `thread.rs`: re-enter guest `thread_init`/`tls_init`, arm mailbox; C++ `main.cpp` installs `imports_set_main_thread_init()` hook |
| 2 | 341 | `env.emscripten_resize_heap(i32)->i32` | C | grow memory; C++ reports 1 because 4 GiB is mapped up front (`IMPORTS.md` §4) |
| 3 | 325 | `env._emscripten_thread_mailbox_await(i32)->void` | C | `mailbox.rs`: condvar waitAsync; C++ defers (host tick, no blocking) |
| 4 | 309 | `wasi_snapshot_preview1.environ_sizes_get(i32,i32)->i32` | C | WASI-lite: empty environment |
| 5 | 310 | `wasi_snapshot_preview1.environ_get(i32,i32)->i32` | C | WASI-lite: zero entries |
| 6 | 313 | `wasi_snapshot_preview1.clock_time_get(i32,i64,i32)->i32` | C | real `clock_gettime` (monotonic / realtime) |
| 7 | 42 | `env._embind_register_void(i32,i32)->void` | C | `embind.rs`: type registry |
| 8 | 43 | `env._embind_register_bool(i32,i32,i32,i32)->void` | C | type registry |
| 9 | 44 | `env._embind_register_integer(i32,i32,i32,i32,i32)->void` | C | type registry |
| 10 | 45 | `env._embind_register_bigint(i32,i32,i32,i64,i64)->void` | C | type registry |
| 11 | 46 | `env._embind_register_float(i32,i32,i32)->void` | C | type registry |
| 12 | 47 | `env._embind_register_std_string(i32,i32)->void` | C | type registry |
| 13 | 48 | `env._embind_register_std_wstring(i32,i32,i32)->void` | C | type registry |
| 14 | 49 | `env._embind_register_emval(i32)->void` | C | type registry |
| 15 | 50 | `env._embind_register_memory_view(i32,i32,i32)->void` | C | type registry |
| 16 | 40 | `env._embind_register_function(8×i32)->void` | C | `embind.rs`: binding table; C++ `embind_bindings()` used by `prepare.cpp` |
| 17 | 4 | `env.emscripten_get_now()->f64` | C | `boot::perf_ms`: monotonic ms (1000 ms origin) |
| 18 | 317 | `env.emscripten_date_now()->f64` | C | wall-clock ms since epoch |
| 19 | 320 | `wasi_snapshot_preview1.random_get(i32,i32)->i32` | C | OS entropy |
| 20 | 7 | `env.emscripten_asm_const_int(i32,i32,i32)->i32` | C | EM_ASM key/signature/packed-arg decoder; logged `key 0x8b2773` unknown but handled |
| 21 | 297 | `env.read_host_info_int(i32)->i32` | P | `host_info.c`: platform/flags from key |
| 22 | 298 | `env.read_host_info_string(i32,i32,i32)->void` | P | `host_info.c`: write string into guest memory |
| 23 | 13 | `env.emscripten_asm_const_double(i32,i32,i32)->f64` | P | same EM_ASM decoder as #7 |
| 24 | 314 | `env.emscripten_console_log(i32)->void` | P | `[WASM-TIMING] initialize() entered` printed here |
| 25 | 1 | `env.emscripten_outn(i32,i32)->void` | I | stdout writer |
| 26 | 333 | `env.__pthread_create_js(4×i32)->i32` | I | `thread.rs`: host thread + mailbox; 9 worker threads spawned here (their start indices are not mapped to symbols — separate issue) |
| 27 | 5 | `env.__asyncjs__rbxFutexWaitMainJspi(i32,i32,f64)->i32` | I | `futex.rs`: bounded host wait, notify hook |
| 28 | 315 | `env.emscripten_console_warn(i32)->void` | I | `[FLog::Output]` Terms-of-Use warning |
| 29 | 311 | `env._tzset_js(4×i32)->void` | I | musl `struct tm` timezone setup |
| 30 | 328 | `env._gmtime_js(i64,i32)->i32` | I | civil time (UTC) |
| 31 | 326 | `env._mktime_js(i32)->i64` | I | civil time (local) |
| 32 | 338 | `env.emscripten_num_logical_cores()->i32` | I | `std::thread::hardware_concurrency()` |

**Stub slots hit during bootstrap: none (rank 0).** The table is complete:
there are no rows to add for zero-returning imports. The last observed guest
action before the trap is `gc_alloc_raw` / `throw_ref` (a builtin), and the
log then ends with `__main_argc_argv: trapped via signal 4` and `RESULT=OK`
(the smoke-test harness treats a controlled trap as a controlled outcome).

## 3. What the task premise got wrong, and what follows from it

The premise "bootstrap reaches `initialize()` before throwing, so stub slots
must be hit next" does not hold in the available traces. `initialize()` throws
immediately after entry, in the same function that calls
`emscripten_num_logical_cores` (#338) and spawns worker threads (#333) — before
GL, WebGPU, fetch callbacks, websockets, OPFS, input callbacks or fibers are
touched. Consequently:

* implementing more stubs is **not** sufficient to get a join;
* the exn-ref/unwind blocker in `vmctx.cpp` must be fixed first;
* after that, the next stub hits are expected from the subsystems the engine
  initializes in `initialize()` — see §4 — but that ordering is derived from
  the reference hosts, not from an observed native trace.

## 4. Stub fixes made in this pass (9 slots)

All nine are promoted in `gen_imports.py::REALIZED` and implemented in the
preserved handwritten region of `imports.cpp`. They need no wasm re-entry and
return *real* host facts (or an honest "unavailable" status), not fabricated
success.

| Slot | Import (signature) | Reference | C++ behavior | Why honest |
| --- | --- | --- | --- | --- |
| 12 | `is_mouse_sourced_event()->i32` | `v2/imports/stubs.rs:29-40` — default pointer type is mouse (1); touch/pen only when a source is installed | returns `1` | native host has no touch/pen source |
| 15 | `emscripten_get_element_css_size(i32,i32,i32)->i32` | `stubs.rs:49-54` — writes `f32` width/height from `gfx::size()` | writes `1280.0f`/`720.0f` (same fixed canvas as slot 206), returns 0 | value reflects the canvas the host actually exposes |
| 55 | `js_web_client_ms_since_navigation()->f64` | `stubs.rs:82`, `boot::perf_ms()` | `wb::steady_ms()` — same 1000 ms origin as slot #4 | real monotonic clock |
| 56 | `js_web_client_wasm_cache_state()->i32` | `stubs.rs:83` — constant `2` | returns `2` | matches the value a successful host reports; no cache hit is claimed |
| 70 | `emscripten_sample_gamepad_data()->i32` | `input.rs:240` — `0` | returns `0` | no gamepads attached |
| 71 | `emscripten_get_gamepad_status(i32,i32)->i32` | `input.rs:242` — `-5` | returns `-5` | genuine "no such device" status, not a fake connected pad |
| 74 | `emscripten_get_num_gamepads()->i32` | `input.rs:240` — `0` | returns `0` | empty device list |
| 329 | `_msync_js(5×i32,i64)->i32` | `stubs.rs:238-240` — `-22` if offset outside ±2^53, else `0` | same range check, else `0` | memory is directly mapped, nothing to flush; offset sanity preserved |
| 330 | `fd_pread(i32,i32,i32,i64,i32)->i32` | `stubs.rs:242-250` — `*nread = 0`, return `0` | clears nread out-param via `wb::mem_st32`, returns `0` | no WASI fd table exists in this host (assets use `js_fetch_file`); "0 bytes read" is factual |

The remaining high-value stubs that are *not* feasible without re-entry or a
real subsystem are listed in §5.

## 5. Remaining stubs and likely blockers (ranked)

Counts after this pass: 83 real + 246 zero-returning + 12 runtime-bridged RNA
= 341. Ranked by "how likely to gate a *successful* boot/join next":

| Rank | Slots | Area | Reference behavior | Why not implemented here |
| --- | --- | --- | --- | --- |
| 1 | — (builtin) | `throw_ref` / `raise` in `vmctx.cpp` | Rust/wasm2c unwind to a catch handler | **actual blocker today**; needs a native exnref/unwind ABI, not an import |
| 2 | 304 (+305–307) | `emscripten_start_fetch` | `http.rs:766` / `fetch.c:256`: dispatch and fire guest `onsuccess`/`onerror` | needs table-index callbacks (`imports_set_wasm_callback`); libcurl half already exists |
| 3 | 85, 76–78, 337 | `emscripten_async_call`, fibers, `__call_sighandler` | `stubs.rs` / `fiber.c`: schedule guest function / stack switch | requires re-entering wasm with a callback id and a fresh stack |
| 4 | 98–205 | OpenGL ES (`gl*`, `emscripten_webgl_*`) | `gl.rs` / `gl_special.c`: real GL calls + handle translation | no GL context in the native host; returning 0/fake values would drive draw-path control flow unchecked |
| 5 | 207–267 | WebGPU | `webgpu.rs` / `wgt.rs`: real wgpu device and object table | same; `wgpuDeviceHasFeature`/texture getters would need a real device |
| 6 | 269–277 | WebSockets | `ws.rs`: real sockets + event callbacks | no browser websocket subsystem and callbacks need re-entry |
| 7 | 278–294 | OPFS | `opfs.rs` / `opfs.c`: async JS layer over filesystem | async JS layer absent; many getters drive control flow |
| 8 | 20–35, 57–75, 81–83 | input / callback registration, text box | `input.rs`: store callback+userdata, return `-1`/`-4` for unsupported | registration itself is cheap, but firing the callbacks is not; a `0` return currently overstates support |
| 9 | 300–301, 303 | stack introspection | `stubs.rs:216-226` returns `0` (no symbols) | could be promoted like #55, but only affects crash-log text; left for a later pass |
| 10 | 79–80, 201–205, 243, 249 | crash reporting, canvas resize, present | `stubs.rs:143-170`: log guest strings and captured backtrace | logging is feasible; present/resize need a real framebuffer; low boot priority |
| — | 86–97 | RNA | `rna.rs` / `rna.c` | **not stubs in a running host**: `rna_bridge.cpp` overwrites their `wasm_call` slots at attach time; leave alone |

The single most misleading remaining stub family for control flow is GL/WebGPU
getters (`glGetIntegerv` #144, `glGetString` #143, `glCheckFramebufferStatus`
#132, `wgpuDeviceHasFeature` #246, texture getters #256–258): their values are
consumed to size buffers, choose formats and branch on errors. They were
deliberately left zero-returning rather than given plausible-looking values.

## 6. Coordination / regeneration notes

* `host/imports.h` is **byte-identical** to HEAD; no declaration was
  hand-edited. Nine names were added to `gen_imports.py::REALIZED` and the
  generator was re-run with the checked-in wasm/docs:
  `python3 host/gen_imports.py --wasm /tmp/opencode/WebClient.raknet.wasm --docs docs/imports-wasm.json --out-dir host`.
  Result: `generated 341 signatures (83 real, 258 stubs)`.
* Regeneration initially dropped two pieces of hand-maintained RNA-bridge code
  that were not part of the generated templates: `wb::imports_set_guest_tick`
  in `imports.h`, and the 12 RNA forwarders (plus `#include "rna_bridge.h"` and
  three forward declarations) in the generated stub region of `imports.cpp`.
  Both were restored **in `gen_imports.py`** (header template, stub-loop RNA
  block extraction, and preamble) so future regenerations keep them. Verified:
  `imports.h` is byte-identical to HEAD, the RNA block and its forwarders are
  present in `imports.cpp`, and `rna_bridge.cpp` still compiles.
* The RNA bodies in slots 86–97 are functionally unchanged (same forwarding
  calls, same trace/log lines); they are not zero-returning stubs in a running
  host because `rna_bridge.cpp` overwrites their `wasm_call` slots at attach
  time. The seven embind setter bodies were not modified.
* Generated stub bodies for the nine promoted slots disappear from
  `imports.cpp` (the generator emits `// slot N: ... -- hand-written below`);
  their new bodies live in the preserved region.

## 7. Verification

* `make` in `host/` — **pass** (warnings only: the pre-existing linker note
  `error in out.full.o(.eh_frame); no .eh_frame_hdr table will be created`).
* `timeout 60s ./demo` — **pass**, `RESULT=OK`, exit 0.
* `nm -C demo` defines all nine promoted `wasm_import_*` symbols.
* Fresh bounded join (`timeout 90s`, `HOST_TRACE=1`) before and after the
  change (final log `host/imports-join-trace-final.log`): 32 first-call lines
  every time, **0 stub hits** every time, same `throw_ref` trap — no
  behavioural regression.
* `host/import_trace.sh --stub-catalog`: 246 zero-returning stubs, 12
  runtime-bridged RNA slots, 83 real.
