# FINDINGS

Reverse-engineering record for the web engine as a RakNet client. Newest first.
Everything here is measured against `WebClient.wasm`
`version-75f1039cb4964863-RobloxWebClient` (100,515,451 B,
sha256 `d72c52618f5c153c9b17a97e…`) on 2026-11-01.

---

## 1. RakNet is compiled in and reachable at runtime

With `FFlagUseRbxTransportClient3=False` injected into the preloaded client
settings, an enrolled place (13569002876) joined the RakNet branch:

```
[DFLog::NetworkClient] Transport selection: useRbxTransportEnabled=false, selectedTransport=RakNet, rccPort=59315, DatacenterId=387
[FLog::Network] UDMUX Address = 128.116.32.34, Port = 59315 | RCC Server Address = 10.18.9.70, Port = 59315
[FLog::Network] getaddrinfo(...) failed:Error code - -4
[FLog::Output] Failed to start network client
[FLog::CreatorOutput] Info: ***ERROR*** Failed to start network client CreatePlayer
[DFLog::NetworkClient] Client:Disconnect
[FLog::Output] Session Transition FSM: Error Occurred.
```

`-4` is musl's `EAI_FAIL`. The engine then keeps running (heartbeat continues),
which means the failure is handled, not fatal to the process.

Consequences:

- The RakNet transport object exists, is constructed, and reads
  `UDMUX Address` / `RCC Server Address` out of the join script.
- The join script handed to the web client still carries the native RakNet
  endpoint fields, so the transport can be selected on the same ticket.
- The first wall is name resolution, not the transport itself.

## 2. There is no socket layer to hook

`wasm-objdump -x` on the module: 268 imports. Network-related imports are only

```
js_fetch_file, emscripten_start_fetch, emscripten_websocket_*
rbx_wasm_rna_{connect,send_datagram,write_stream,open_stream,...}_js
```

No `socket`, `connect`, `sendto`, `recvfrom`, `bind`, `setsockopt`, `poll`,
`select`, and no `__syscall_socket` family (only `geteuid/getegid/getuid/getgid`).
`getaddrinfo`, `sendto` and friends exist only as strings/format literals.

Therefore: RakNet cannot be "unblocked" by a flag or a function pointer. The
bytes must be moved by replacing the libc entry points themselves.

## 3. The app layer is transport-independent

RbxTransport channel streams carry `[compactVarint len][type][body]` frames —
the same framing `run/repl_1818.bin` uses and the same decoder already in the
Boblox tree (`compactVarintDecode`). Captured live from the enrolled place:

```
head=14  len=1816  type=93  | 93 00 2f 00 1a 00 "UserPlayerScriptsRefactor2..."
head=85  len=976   type=ba  | ba e5 21 00 00 00 ba 03 00 00 93 07 00 00 28 b5 2f fd ...
```

- `0x93`/1816 B matches the message decoded in Boblox FINDINGS.
- `type=ba` is a compressed batch `[u32][u32 comp][u32 decomp][zstd]`
  (`28 b5 2f fd` = zstd magic) — the RbxTransport twin of `0x83/0x16`.

Channel control rides the same stream: `06 01 04 'ctrl'` (open, named),
`02 <u32 app> <u32 channelId> <u32 wireId>` (open unreliable).

Relevance: the engine's own RakNet client carries the same application messages,
so a patched transport needs no translation layer above it.

## 4. Function reachability for patching

```
Export[49]:  table[0] -> "__indirect_function_table"
Elem[1]:     segment[0] table=0 count=174300 init i32=1
Code[167913] (~91 MB), Data[2994 segments] (~8 MB)
```

Every internal function sits in the exported table, so from JS
`exports.__indirect_function_table.get(i)` is a callable entry point. Reachability
is not the problem for patching; identification is. That is what
`tools/wasm_scan.py` exists for: resolve a function by a string anchor, never by
a fixed offset.

## 5. The libc stub block — `getaddrinfo` located

`func[301]`, 8 instructions:

```
local.get 0 ; if ; local.get 0 ; i32.const 0 ; i32.store 2 0 ; end ; i32.const 4294967292 ; end
```

That is, verbatim:

```c
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
  if (res) *res = NULL;
  return EAI_FAIL;            /* -4 */
}
```

Matching the live log (`Error code - -4`). Its neighbours are the same family:

| func | body | meaning |
| --- | --- | --- |
| 298 | `call errno_loc; *errno = 8; return -1` | `EBADF` stub (`close`-family) |
| 299 | `call errno_loc; *errno = 138; return -1` | `ENOSYS` stub — the socket catch-all |
| 300 | `end` | empty void stub |
| 301 | see above | `getaddrinfo` |

Everything socket-related bottoms out here. The engine does not call these
directly; it calls libc entry points that do (callers of the stubs live in the
127xxx libc region).

**How it was found, and why it generalises:** string xrefs do not work in this
module — literals are interned into one blob (`0x2b14c4` points into the middle
of `"...torAngles (ignored): %s\0[FLog::Network]"`), so `i32.const <addr>` almost
never names a string. Function *shape* does work: scan for a short body whose
observable behaviour is the one you saw on the wire (returns `-4`, nulls an
out-param). `tools/wasm_scan.py` does the section/body bookkeeping; the search
is a filter over parsed bodies.

## 6. Tooling caveat

`wabt 1.0.36` (distro package) cannot decode this module: it fails at offset
0x63 with `expected valid result type (got -0x17)` — the build uses post-MVP
encodings (`exnref`/exception handling). A current wabt or Binaryen is required
for full disassembly; the scanner only needs section/function boundaries and raw
byte scans, which work today.

## 7. Retracted / corrected

- "The wasm's RakNet code is dead because there are no sockets." — wrong. The
  code runs; it is the socket backend that is missing (§1).
- "You cannot lift the engine's challenge/Luau out." — wrong in the literal
  sense: every function is table-reachable (§4). The obstacle is engine state,
  not addressability.
