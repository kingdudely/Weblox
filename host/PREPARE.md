# Porting `prepare()` to the C++ host (`feat/cpp-prepare`)

## Goal

The browser host (`WasmPlayer.js`) calls `prepare()` before `__main_argc_argv`.
The Rust host's version is `v2-wasmtime/src/main.rs:770`; the C++ host entered
`initialize()` without it and threw before any RNA import could run. This
branch adds `host/prepare.h/.cpp`, calls it from the join path in `host/main.cpp`,
and documents the result. `host/PREPARE.md` is this file.

## Exact sequence now executed

`weblox::prepare(state, place_id, cookie, meta_path, content_base)` is called
**after** `bridge.attach()` / `bridge.set_config()` and **before** `guest_argv()`
and `__main_argc_argv` (`host/main.cpp`, join path only; `state.vmctx` is the
store/vmctx). It does the following, in the same order as the Rust port:

1. **embind setters**, each through `wb::embind_find(name)` -> binding `invoker`
   table slot -> `wb_rna_invoke(callee_vmctx=state.vmctx,
   caller_vmctx=state.vmctx, fn=table_funcrefs[invoker].wasm_call, args, n)`.
   Argument 0 is always the binding id; string arguments are the embind
   `std::string` wire form (`u32 length`, then bytes), built with guest
   `malloc`. The two-string form is `set_account_locales("en-us", "en-us")`.
   * `set_pretend_platform("desktop")`
   * `set_roblox_base_url("https://www.roblox.com/")`
   * `set_channel("")`
   * `set_browser_tracker_id("")`
   * `set_account_locales("en-us", "en-us")`
   * `set_preloaded_flags(JSON)` — string built with the trailing NUL like
     `binds::guest_string_bytes()`
   * `set_place_id(i64)` — **one 64-bit wasm argument**: `wb_rna_invoke` loads
     each `args[]` entry as 64 bits, so the `i64` lands in `rcx`. Verified
     against the registered signature `vpj` (`j` = i64).
2. **Settings JSON**. Minimal but valid object, with the two flags assigned
   last (last-wins, same assignment as `client/page/env.ts:608-610`):
   ```json
   {"applicationSettings":{"BaseUrl":"https://www.roblox.com/",
   "FIntWasmMemoryPages":"32768","FFlagUseRbxTransportClient3":"False",
   "FFlagUseRbxTransportServer":"False"},"RobloxLocaleId":"en-us",
   "GameLocaleId":"en-us"}
   ```
3. **VFS registration**: parses `data/meta.json` (resolved to
   `../v2-wasmtime/data/meta.json` from `host/`, or `WC_META`) exactly as the
   Rust `field()` helper does — split on `{`, read `"path"` as a quoted string
   and `"size"` as digits (default 1). For every entry, `import_bytes(path)`
   (NUL-terminated guest string) then a direct call to the exported
   `rbx_fs_register_asset` (wasm index 373 = `_wasm_function_32` after the
   341 imports). 5142 paths registered in the verification run.
4. **InExperience preload**: for `/models/InExperience/InExperience.rbxm` and
   `/models/InExperience/InExperience_checksum`, recursively looks under
   `content_base` (`WC_CONTENT` override, else `data/content`) for a file whose
   path ends with the tail (mirroring Rust `find_under`). When found, re-register
   the path with the real byte length, copy the bytes into guest memory, and
   call exported `rbx_fs_preload_asset` (index 385 = `_wasm_function_44`).
   The mirror is gitignored and **absent** in this worktree, so both files were
   skipped (logged, not fatal) — `preload=0`.
5. **`rbx_fs_add_file`** (index 387 = `_wasm_function_46`) for
   `/appData/LocalStorage/appStorage.json` with
   `{"WebLogin":"2","RobloxLocaleId":"en-us","GameLocaleId":"en-us"}`.
   The Rust port passes a hardcoded length of 61, which truncates the 64-byte
   JSON; this port passes the actual length (64) so the engine sees valid JSON.
6. **`rbx_fs_set_base_url`** (index 372 = `_wasm_function_31`) with
   `https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient/`.
7. **`guest_argv()`**: `malloc(16)`, `"/content/.keep\0"`, `malloc(8)`, then
   `argv[0]=str`, `argv[1]=0` in guest memory — identical to
   `v2-wasmtime/src/main.rs:756`. Returns the array pointer used as
   `__main_argc_argv(1, argv)`.

Direct export indices were taken from `wasm-objdump -x
/tmp/opencode/WebClient.raknet.wasm` (`rbx_fs_set_base_url`=372,
`rbx_fs_register_asset`=373, `rbx_fs_preload_asset`=385,
`rbx_fs_add_file`=387, `__main_argc_argv`=488, `main_loop_iteration`=517);
the linked symbols subtract the 341 imported functions. Signatures checked
against the type section: `(i32)->void`, `(i32,i32)->i32`, `(i32,i32,i32)->i32`.

`--skip-ctors` deliberately skips prepare (there is no embind table), and
`--rna-probe` returns before prepare — both paths were re-verified. A
`--bindings` diagnostic was added to `main.cpp` to print the registered
setters without invoking them.

## Ported vs omitted

**Ported:** all seven embind setters, last-wins flags JSON, place id as i64,
5142 VFS registrations, appStorage file, asset base URL, guest argv, and the
optional InExperience preload path.

**Omitted / deliberately different:**
* Live settings fetch via `curl` to `clientsettingscdn.roblox.com` (Rust
  `settings()`). This uses its `MIN_RAK` fallback verbatim. `WC_NOREG` and the
  `WC_RAKNET` `/tmp/opencode/settings-sent.json` dump are not implemented.
* The two InExperience asset bodies: no `data/content` mirror exists here;
  the code tolerates the absence and logs it.
* No fibre/parked-task scheduler, no native exception unwinder, no GC
  collection — these are outside this branch (see `host/GC.md`).
* `cookie` is accepted in the signature for parity with the host call site but
  is not used or logged by prepare (the join route fetch already consumed it).

## Verification (all bounded; one join attempt)

```sh
cd /tmp/opencode/wt-cpp-prepare/host
make -j4                       # demo rebuilt with prepare.cpp
timeout 60s ./demo
timeout 60s ./demo --skip-ctors
timeout 60s ./demo --bindings
WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt timeout 90s ./demo --rna-probe
HOST_TRACE=1 WC_PLACE=189707 WC_COOKIE_FILE=/home/john/mizg.txt \
  WC_LOOP_MS=3000 timeout 90s ./demo
```

`make` succeeded (only the pre-existing harmless `.eh_frame_hdr` linker
warning). `./demo` exited 0 with `RESULT=OK`, `__wasm_call_ctors` returned
normally, `controlled errors=0`. `./demo --skip-ctors` exited 0 with
`RESULT=OK` and `controlled errors=0`. `./demo --bindings` showed all seven
bindings live, including the crucial i64 signature:

```
[bindings] set_pretend_platform       sig=vpp      argc=1 invoker=263 id=275 table=1
[bindings] set_roblox_base_url        sig=vpp      argc=1 invoker=263 id=264 table=1
[bindings] set_channel                sig=vpp      argc=1 invoker=263 id=272 table=1
[bindings] set_browser_tracker_id     sig=vpp      argc=1 invoker=263 id=271 table=1
[bindings] set_account_locales        sig=vppp     argc=2 invoker=267 id=268 table=1
[bindings] set_preloaded_flags        sig=vpp      argc=1 invoker=263 id=265 table=1
[bindings] set_place_id               sig=vpj      argc=1 invoker=269 id=270 table=1
```

`--rna-probe` (with the two env vars) exited 0:
`probe ticks=587 trap=0 connects=1 status=200 callbacks=1/0 ring=0/0` — the
bridge callback path is unchanged.

The single bounded join run (log: `/tmp/opencode/wt-prepare-join.log`) exited 0
and reached:

```
[prep] set_pretend_platform id=275 invoker=263 sig=vpp argc=1 n=1
...
[prep] set_place_id id=270 invoker=269 sig=vpj argc=1 n=1
[prep] registered 5142 asset paths (meta=../v2-wasmtime/data/meta.json)
[prep] no mirror file for /models/InExperience/InExperience.rbxm
[prep] no mirror file for /models/InExperience/InExperience_checksum
[prep] done: ok=1 place=189707 assets=5142 preload=0 files=1 base=https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient/
[rna-main] prepare ok=1 trap=0 meta=data/meta.json content=data/content
[rna-main] __main_argc_argv argc=1 argv=27604456 (argv_trap=0)
[log ] [WASM-TIMING] initialize() entered: +1491.0ms from preInit
[log ] [WASM-TIMING] RunDynamicInitializers: +1535.6ms from preInit, phase took 44.5ms
[log ] [WASM-TIMING] CrashReporter init: +2546.6ms from preInit, phase took 1011.0ms
[log ] [WASM-TIMING] gameGlobalInit: +2629.6ms from preInit, phase took 82.9ms
[log ] [WASM-TIMING] LoadFlags: +2638.3ms from preInit, phase took 8.8ms
[log ] [WASM-TIMING] checkMimallocIntegration: +2638.4ms from preInit, phase took 0.1ms
Base URL: https://www.roblox.com/
Base Host: .roblox.com
[warn] ... [FLog::ClientRunInfo] The base url is https://www.roblox.com/
[warn] ... [FLog::ClientRunInfo] The channel is production
[trace] gc_alloc_raw kind=0x4000000 ty=894 size=40 align=8 -> heap_index=8
[trace] throw_ref exnref=0x8 -> not caught
[wasm-host] controlled error: throw_ref called (exception propagation not implemented)
[trace] raise builtin -> ud2 (no native unwinder)
[rna-main] __main_argc_argv: trapped via signal 4
[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
[wasm-host] controlled errors=1
```

The guest logged the base URL and channel that `prepare()` set, and got
through `LoadFlags` — measurably further than the pre-port trace in
`host/GC.md`, which failed immediately after `initialize() entered`.

## Result / stop condition

`prepare()` itself succeeded (`ok=1 trap=0`, no missing bindings), and the
engine no longer throws at the start of `initialize()` for missing settings.
**`throw_ref` still fires**, at the same `kind=0x4000000` (ExnRef) 40-byte
allocation / shared type 894, now after `LoadFlags` and `checkMimallocIntegration`.
It reaches `raise` -> `ud2`, converted to signal 4. **RNA imports still do not
run**: `connects=0`, `callbacks=0/0`, no `rbx_wasm_rna_*` import calls. The
blocker is exactly the one recorded in `host/GC.md`: Wasmtime implements
exceptions with a pending exception plus native unwinding through `try_call`
frames; this host has no unwinder/exception-table registration, so
`throw_ref` cannot reach a catch handler. Per the task's stop condition, work
stopped here — implementing an unwinder is out of scope for this branch.
