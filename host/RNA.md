# RNA bridge: native WebTransport behind the wasm `rbx_wasm_rna_*` imports

This module wires the 12 `rbx_wasm_rna_*` imports (slots 86–97 of
`host/imports.h`) to `weblox::WebTransport` (`host/wt.h`, `wt.cpp`) and
`weblox::JoinRoute` (`host/route.h`, `route.cpp`). The prior Rust implementation
in `v2-wasmtime/src/imports/rna.rs` is the reference for the ring/callback
protocol; the page implementation is
`docs/reference/research/WebClient.pretty.js` (`oa`, `sa`, `_a`, `va`, and the
deferred-callback switch).

## Files

| File | Role |
| --- | --- |
| `host/rna_bridge.h/.cpp` | `weblox::RnaBridge`: connection map, receive rings, pump thread, event queue, guest callback re-entry |
| `host/imports.cpp` | slots 86–97 forward to the bridge; slot 4/5 call `wb::g_guest_tick` |
| `host/imports.h` | `imports_set_guest_tick()` hook |
| `host/main.cpp` | `--rna-selftest`, `--rna-probe`, and optional join mode via `WC_PLACE` / `WC_COOKIE_FILE` |
| `host/Makefile` | links `wt.cpp`, `route.cpp`, `rna_bridge.cpp` and libquiche |
| `host/RNA.md` | this document |

`host/wt.*`, `host/route.*`, `host/vmctx.*`, `host/gen_*.py`, `host/table.h`
and `host/runtime_data.*` were not modified.

## Protocol mapping

* **Slot 96 `connect_js`** — arguments are
  `(id, url*, certs*, certCount, data, head, tail, slot, count, errOut,
  fixOwnership, dgCompat, ...)`. The URL is
  `https://<game-fqdn>:<port>/wt?X%2DRbx%2DCapabilities=<hex>&...`; the bridge
  parses FQDN, port and caps from it, reads the guest's base64 cert-hash array,
  takes the UDP peer and the fallback pin from a `JoinRoute`, calls
  `WebTransport::connect()` (blocking, synchronous CONNECT until `:status 200`)
  and stores the connection by id. `WebTransport::send_datagram()` adds the
  qdmux session-id varint itself; the bridge never double-prefixes.
* **Receive rings** — `tryWriteReceiveRingSlot` semantics from the page:
  `head = Atomics.load(headPtr)`, reject when `head - tail >= count`, copy
  `min(len, slot)` bytes to `data + (head % count) * slot`, `head++`.
  Datagrams use kind 1 / stream id 0. Server-initiated streams use kind 0 and
  RNA stream ids 3, 7, 11, … (`nextIncomingStreamId += 4`), matching the page.
* **Slots 86–97** — `open_stream_js` calls `wt::open_uni_stream()` (WT_UNI
  header written in `wt.cpp`) and replays writes that arrived before the QUIC
  stream existed; `write_stream_js` calls `send_stream_data()` and queues
  `on_stream_write_success/error`; `pause/resume/shutdown_read/consumed`
  update the per-stream pause/stop sets; `close_js`,
  `send_connection_close_js` close the native session.
* **Completions** — the pumper thread never re-enters wasm. It writes payloads
  into the guest rings and pushes events. `wb::imports_set_guest_tick()` is
  called from imports 4 and 5 (guest-thread clock/futex), which delivers **one**
  completion per turn. `on_connect_success`, `on_connect_error`,
  `on_stream_accepted`, `on_stream_fin/error`, `on_stream_write_success/error`,
  `on_connection_closed`, `on_close_complete`, `on_datagram_acked/lost` go
  through the embind invoker recorded by `_embind_register_function`
  (`Binding.invoker`, first argument `Binding.id`) at the original table index.
  Receive-ring slots go through the engine's per-slot consumer at table index
  `135389` with a 24-byte guest task `{conn, kind, sid, pad, slot, len}`,
  exactly as in `rna.rs`; the embind `on_receive_ring_slot` callback is the
  fallback. The Wasmtime-50 wasm-call ABI is handled by the `wb_rna_invoke`
  asm shim (`rdi`=callee vmctx, `rsi`=caller vmctx, args from `rdx`, stack args
  callee-popped).
* **`shutdown_stream_write_js` is an honest stub**: `wt.cpp` has no FIN-capable
  send, so the bridge logs instead of pretending the peer received a FIN.

## Build

`libquiche` is built out of tree by `host/build_quiche.sh` (first build ~6 min,
incremental afterwards). The Makefile reads `link.mk`, falling back from
`QUICHE_PREFIX=/tmp/wl-qtry-prefix` to `/tmp/weblox-quiche`:

```sh
./host/build_quiche.sh                       # -> /tmp/weblox-quiche/link.mk
cd host && make -j4                          # -> host/demo (not committed)
timeout 60s ./demo                           # smoke test
```

Link line (from the built `link.mk`):

```
g++ -std=c++17 -O1 -g -I. -I/tmp/wl-qtry-prefix/include -o demo \
  main.cpp vmctx.cpp imports.cpp rna_bridge.cpp wt.cpp route.cpp \
  runtime_data.cpp table.cpp /tmp/opencode/out.full.o \
  -rdynamic -lpthread -ldl -lm -lcurl \
  -L/tmp/wl-qtry-prefix/lib -lweblox_quiche
```

`table.cpp` and `runtime_data.cpp` are regenerated from
`/tmp/opencode/WebClient.raknet.wasm` at build time and stay out of git.

## Bounded verification (all runs `timeout 60s`)

### 1. No regressions in the smoke test

```
./demo                 -> exit 0, "__wasm_call_ctors returned normally", RESULT=OK
./demo --skip-ctors    -> exit 0, RESULT=OK
./demo --test-trap     -> exit 0, "__trap: converted to controlled error", RESULT=OK
```

### 2. Native CONNECT through the bridge (`./demo --rna-selftest`)

Fresh route fetched in-process with `WC_PLACE=189707` and
`WC_COOKIE_FILE=/home/john/mizg.txt`, then one native CONNECT + 6 s pump:

```
[rna-main] route: fqdn=a9bd1475...qdmux.roblox.com host=128.116.2.34 port=56850 caps=120ddbc750b31896 cert=KXK9xMCiky+gnLzGOtRsJjYBe6BYdlWQSDjEYdRQGhY= place=189707 status=2
[rna ] attached: memory=4294967296 bytes table=161491 entries
[rna-main] selftest connected=1 status=200 open=1 packets_sent=32 packets_recv=32 pings=2 ring_slots=0 ring_drops=0 callbacks=0/0
```

No wasm callbacks in selftest mode by design (no guest `connect_js`).

### 3. One bounded join attempt (`WC_PLACE=189707 WC_COOKIE_FILE=... ./demo`, `WC_LOOP_MS=3000`)

```
[rna-main] route: ... status=2
[rna ] attached: memory=4294967296 bytes table=161491 entries
[rna-main] __main_argc_argv argc=1 argv=27189176 (argv_malloc_trap=0)
[import] [asm] unknown key 9099790
[log ] [WASM-TIMING] initialize() entered: +1477.6ms from preInit
[wasm-host] controlled error: gc_alloc_raw called (GC not implemented in skeleton)
[wasm-host] controlled error: unimplemented builtin called
[rna-main] __main_argc_argv: trapped via signal 4
[rna-main] final: connects=0 connect_errors=0 callbacks=0/0 status=0 error=
```

The guest reached `initialize()` and then trapped (SIGILL) in a GC allocation /
unimplemented builtin **before calling any RNA import**. This is the current
blocker for a full join; no game session was attempted.

### 4. Callback re-entry probe (`./demo --rna-probe`)

A synthetic `connect_js` with a real guest receive ring exercises the native
CONNECT plus the guest callback path without engine startup. Fresh route,
`WC_LOOP_MS=3000`, verbatim from `/tmp/wt-cpp-rna-probe3.log`:

```
[rna-main] route: fqdn=a040b07c1ff686ddd35a03fecfaae5eb-0a2500c6-edd5.80740222.qdmux.roblox.com host=128.116.2.34 port=60885 caps=120ddbc750b31896 cert=MpNTEqsQL2biGTVVk/J5lK/d7Joqg2estdLuNeb3Vp0= place=189707 status=2
[rna ] attached: memory=4294967296 bytes table=161491 entries
[rna-main] probe url=https://a040b07c1ff686ddd35a03fecfaae5eb-0a2500c6-edd5.80740222.qdmux.roblox.com:60885/wt?X%2DRbx%2DCapabilities=120ddbc750b31896&X-Rbx-Close-Reason=utf8-v1 ring=27189256/27451400/27451404
[rna ] connect id=1 url=https://a040b07c1ff686ddd35a03fecfaae5eb-0a2500c6-edd5.80740222.qdmux.roblox.com:60885/wt?X%2DRbx%2DCapabilities=120ddbc750b31896&X-Rbx-Close-Reason=utf8-v1 caps=120ddbc750b31896 certs=0 ring(data=27189256,head=27451400,tail=27451404,slot=65536,count=4)
[rna ] -> rbx_wasm_rna_on_connect_success id=1
[rna-main] probe ticks=570 trap=0 connects=1 status=200 callbacks=1/0 ring=0/0
```

This is the important result: the guest's embind invoker at the recorded table
index was re-entered from the host tick hook with the wasm-call ABI, delivered
`rbx_wasm_rna_on_connect_success(id=1)`, and produced **no trap** across 570
ticks. An earlier probe with the same shape timed out at CONNECT because the
URL parser used an authority-relative colon index as an absolute offset into
the URL (FQDN lost its first 8 characters); that bug is fixed in
`rna_bridge.cpp` and the corrected run above is the evidence.

`ring_slots=0` is expected: no engine code ran, so nothing spoke the Roblox
game protocol and the server had no reason to send qdmux datagrams/streams.

## What works / what remains

**Works:** route fetch + derivation, QUIC/H3 extended CONNECT `:status 200`,
certificate pin, keepalive/pump thread, qdmux datagram framing, WT stream
headers, ring writes, event queue, guest-thread callback delivery through the
embind invoker and the wasm table, and no regressions in the existing smoke
test.

**Remaining for a full in-game join:**

1. Guest startup needs `gc_alloc_raw` and several builtins that `vmctx.cpp`
   stubs out (blocker proven above).
2. Engine bootstrap (`prepare()` in the Rust port) is not ported: preloaded
   flags, place id, asset VFS, locale settings and `guest_argv` are missing.
3. The C++ host has no fibre/parked-task scheduler; completions are delivered
   one per guest clock/futex turn, which may still need the page's deferred
   callback queue for multi-step engine handlers.
4. FIN-capable stream writes, per-stream flow control and full H3/WT stream
   demultiplexing are not implemented in `wt.cpp`.
5. Route expiry: routes last minutes; join mode fetches a fresh one per run.
