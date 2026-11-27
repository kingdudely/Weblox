# Research archive

Notes from the retired host experiments. The supported host is **`client/` (Deno)** —
see the root [README](../README.md). Nothing here is needed to run the client;
it exists so the reverse engineering is not lost.

## Archived branches

| Branch | Commit | Contents |
| --- | --- | --- |
| `experiment/wasmtime` | `8fbda9b` | Full WIP: WebTransport backend (`wt.rs`), fibre scheduler, 342-import Rust layer |
| `experiment/old-scheduler` | `ff15c48` | Earlier fibre-scheduler variant |
| `experiment/webtransport` | `0b5d6ae` | Verified WebTransport join + `WC_NATIVE_JOIN_UA` A/B flag (the useful part is on `master`) |
| `experiment/socket-patch` | `07f29c4` | `patch_new.py`: byte-level RakNet socket patches for the new engine build |
| `experiment/udp-host` | `0c812f9` | `raknet.rs`: native-UDP ring pump + RUPP port |

These branches are local only. Do not merge them into `master`.

---

## 1. The "WebTransport session gate" was a User-Agent problem

`client/INVESTIGATION.md` concluded that Roblox accepted QUIC but never opened
RbxTransport streams. That was wrong. The engine sent:

```
rbx-user-agent: Roblox/Web Unknown (...) Browser/Chromium/157.0.0.0 RobloxApp/0.748...
```

gamejoin replies to that identity with **status 12, "Playtesters Only"**. With a
native identity the same request returns **status 2** and a full join script:

| Request headers | Result |
| --- | --- |
| browser `User-Agent`, no `rbx-user-agent` | status 12 |
| browser `User-Agent` + `rbx-user-agent: Roblox/Darwin` | status 2 (derived `gameFqdn`) |
| `User-Agent: Roblox/WinInet` + `rbx-user-agent: Roblox/WinInet` | status 2, real `UdmuxEndpoints`, no `gameFqdn` needed |
| native UA + engine's `...Browser/Chromium...` | status 12 |

Once corrected, WebTransport joins end-to-end: `CONNECT 200`, 12 incoming
streams, schema init, `Assigned peer id`, `#players = 21` on place 189707.

Verified join latency on place 189707: `joinGameSucceeded` ≈ 8.2 s,
`Assigned peer id` ≈ 9.2 s from process start.

## 2. Why RakNet was retired

`version-1ef59424f70d4932` (the build the Deno client uses) contains the full
RakNet implementation — 294 `RakNet` / 19 `RakPeer` string hits, same functions
as the older Weblox build — but refuses to select it at runtime:

```
Transport selection rejected: rakNetDisabled=true, useRbxTransportEnabled=false,
selectedTransport=None, rejectionReason=RbxTransportDisabled, eligibilityEvaluated=false
```

- `rakNetDisabled=true` is **literal text in the log format string**, not a
  printed variable. `ClientRakNetDisabled` / `RbxTransportDisabled` are
  rejection-reason enum labels, not FFlag names.
- No live settings application (`WebClientApp`, `WindowsWebClientApp`,
  `MacWebClientApp`, `PCDesktopClient`) contains any `*RakNetDisabled*` key.
  Injecting every plausible name as `"False"` or JSON `false` had no effect.
- The deciding predicate is **`func[17777]`** (36 bytes at file offset
  `0x117c06c`):

  ```wasm
  local.get 0; i32.eqz; if
    i32.const 0; i32.load8_u <0x810db4>; i32.const 1; i32.and; i32.eqz;
    i32.load8_u <0x85e324>; i32.and; return
  end
  i32.load8_u <0x85e324>
  ```

  `mem[0x810db4]` (the new gate byte) is read **only here**.
  `mem[0x85e324]` is `UseRbxTransportClient` (77 reads).
- `func[8043]` calls `func[17777]` and, when it returns 0, emits the rejection
  through `func[7984]`. `func[8043]` is called only from `func[9281]`.
- Minimal patches found (never applied to `master`):
  - global: rewrite the 36-byte body of `func[17777]` to `i32.const 1`.
  - surgical: at file offset `0x7880ee`, replace
    `10 f1 8a 01 45` (`call 17777; i32.eqz`) with `41 00 01 01 01`.
  - runtime alternative: host writes `mem[0x810db4] = 0` after ctors.
- The gate byte is **not in any data segment**, so no one-byte file patch works.

Even with the gate flipped, the module has no socket imports and `getaddrinfo`
still returns `-4`, so Weblox-style socket patching plus a ring/RUPP host is
required. That maintenance burden, plus an explicit platform decision by
Roblox, is why this was dropped.

## 3. Socket patch sites for `version-1ef59424f70d4932`

Absolute function indices (341 imports included). See
`experiment/socket-patch` for the tool. `wasm-tools validate --features all`
passes on the patched output.

| Site | Old (`75f1039c`) | New (`1ef59424`) |
| --- | --- | --- |
| EBADF stub | 298 | **395** |
| ENOSYS stub | 299 | **397** |
| getaddrinfo | 301 | **398** |
| udp_send | 127931 | **117578** |
| recv loop | 127685 | **117378** |
| port check | 127810 | **117374** |
| UpdateNetworkLoop | 127660 | **117377** |
| RakPeer::Connect | 127655 | **117465** |
| malloc export | 163490 | **150325** |
| `emscripten_console_log` import | 246 | **313** |
| defined function count | 167913 | **154550** |

Stub bodies are byte-identical between builds. Errno constants in the bytes
are **8** (EBADF) and **138** (ENOSYS), not the 9/38 in Weblox's comments.
Splice patterns in `func[117378]`: recv at body+`0x4ea`
(`20 02 10 8b 03`), select at body+`0x4c9` (`37 03 c0 01 10 8b 03`).
Portsub in `func[117374]` at body+8641: `41 c0 e1 93 04 28 02 00` → `41 00`.
Appended stubs become `func[154891]` / `func[154892]`. Ring ABI unchanged;
slot stride **1536** (`patch.py`/`abi.js`).

The ring is at linear `0x7FF00000`: magic 0, TX head/tail 0x04/0x08, RX
head/tail 0x0c/0x10, wake 0x14, addr 0x20, host 0x40 (48 B), port 0x70,
TX0 0x80, 64 slots × 1536 B. `experiment/udp-host` implements the host half
with blocking `UdpSocket` threads and a RUPP port of `rupp.js`.

## 4. Runtime backtrace technique

Useful for locating any log-producing branch in a stripped wasm:

1. Hook `emscripten_console_log` (and `_emscripten_log_formatted`) in the host.
2. On a target message, call `wasmtime::WasmBacktrace::force_capture(&c)` —
   works from inside `func_wrap`, no trap needed.
3. Map `FrameInfo::func_index()` through a WAT dump (`(func (;N;)` lines).
4. In Deno/V8, `new Error().stack` inside a console hook prints
   `wasm-function[N]:0x<offset>`; V8 prints all indices as 0, so map module
   offsets to code-section bodies instead.

Passive data strings have no direct `i32.const` xref — e.g. the rejection
string at linear `0x192b17` is reached through a pointer table. Shape scans
and byte patterns are more reliable anchors than strings.

## 5. Runtimes and approaches ruled out

- **wasm2c**: generated C is machine code with opaque names; a 3 GB translation
  regenerates weekly. Measured guest CPU was only 2–10%, so AOT gains were
  irrelevant. Parked on the local `experiment/wasm2c` branch.
- **Raw V8 C++ host / wee8**: `FunctionTemplate` imports are JS-callable under
  the hood; wee8 hardcodes `SharedFlag{false}` (no pthreads) and rejects
  exception tags. Full V8 has no public API to create a `WasmMemoryObject`
  from an existing shared backing store across isolates.
- **WASI sockets**: the module has only 6 preview1 calls (`proc_exit`,
  `environ_*`, `clock_time_get`, `random_get`, `fd_pread`) — no `sock_*`.
- **RNA imports for RakNet**: those are the RbxTransport/WebTransport protocol;
  feeding RakNet through them needs a gateway, strictly harder than patching.
- **Emscripten C++ host implementations**: none exist. The C++ side (`system/lib`)
  is compiled into the wasm; the host-facing functions are JS glue in
  `src/lib/lib*.js`. Only the 6 WASI calls have ready-made hosts.
- **V8 vs wasmtime**: V8 does speculative `call_indirect` inlining automatically
  at the top tier (default on since Chrome 137), measured 1–8% on real workloads.
  Cranelift does not. But CPU was never the bottleneck.

## 6. Environment gotchas

- Use a **valid `.ROBLOSECURITY`**. A stale cookie reliably hangs at
  `[pthread] thread #1 created` with heartbeats continuing; it is not a code
  bug. `king.txt` went stale; `mizg.txt` was known-good at the time of writing.
- Some places intermittently stall at the same point; retrying the identical
  command usually succeeds. Place 1818 became consistently unreliable after
  many retries, while 189707 kept working — likely per-place rate limiting.
- Place 1818 did not emit `#players` even after 115 s; place 189707 reports
  `#players = 21`. Use 189707 for join comparisons.
- `wabt 1.0.36`'s `wasm-validate` rejects this module at file offset `0x60`
  (unsupported valtype). Use `wasm-tools validate --features all`.
