# RakNet datagram seam (native host)

`host/raknet.{h,cpp}` is the C++17 port of
`alcohol/experiment/udp-host:v2-wasmtime/src/raknet.rs`, cross-checked against
`iwa/ring.js`, `iwa/rupp.js` and the ring splice in `tools/patch.py`.

The patched guest has no sockets. `sendto` copies a datagram into a TX ring in
shared linear memory; `select`/`recvfrom` park on a futex word and drain an RX
ring. This module is the other end: **one UDP socket, one TX pump thread, one
RX pump thread**. It knows nothing about RakNet packets beyond the three RUPP
fields a join script owns.

No game runs were performed for this change. The seam is unit-verified against
a local UDP echo socket and synthetic join scripts; integration with the native
host's VMContext is a separate worker's task.

## Files and API

Only `host/raknet.h`, `host/raknet.cpp`, and this document belong to this seam.
The header is namespace `weblox::raknet`; global aliases `raknet` and `rk` are
provided for host code that does not use namespaces.

```cpp
namespace weblox::raknet {

JoinTarget parseJoin(const std::string& script);  // throws on bad peer
class Rupp {
 public:
  Rupp& setJoinScript(const std::string& script);  // best effort
  void stamp(uint8_t* data, size_t len);
  void learn(const uint8_t* data, size_t len);
  // join(), session(), rcc(), data_port() accessors for logging/tests
};

struct Stats { uint64_t tx, rx, dropped, errors; };
Stats stats();
void stop();       // nonblocking: pumps exit at next poll/recv timeout
void shutdown();   // stop + join threads + close socket; allows a later start()

struct StartOptions {
  size_t memory_size = kMemorySize; // 2 GiB, bounds all guest-memory accesses
  uint32_t slot = 0;                // 0 => WC_RK_SLOT env or kSlot (1536)
  bool arm_connect = true;          // set CREQ=1 once pumps are live
};

void start(const std::string& host, uint16_t port,
           const std::string& script, uint8_t* guest_memory_base,
           std::function<void()> wake = {}, const StartOptions& options = {});
}
```

`start` binds one UDP socket to `0.0.0.0:0` (IPv4 peer) or `[::]:0` (IPv6
peer) and keeps it for the entire session: a local port that changes per send
would look to the RCC like the session hopping hosts. Socket read/write
timeouts are 250 ms. Failures throw `std::runtime_error` or
`std::system_error`; the "already started" flag is released on failure.

The module is deliberately **VMContext-independent**. `guest_memory_base` is a
raw pointer to guest linear-memory address 0; `memory_size` bounds every
access. The `wake` callback is invoked from the RX thread after a datagram is
published, and the integrator should make it notify the guest futex at
`RING_BASE + WAKE` with count 1 (e.g. a Wasmtime `atomic_notify` through the
host's VMContext). An empty callback simply skips the notify; the guest's
`select()` still polls every 1 ms timeout. Empty `host`/`port` arguments are
filled in from `parseJoin(script)`; a malformed script is tolerated when an
explicit route is supplied (the RUPP then stays unstamped).

`std::atomic<uint32_t>` is used for the head/tail/length words (acquire loads,
release stores) to match the guest's `memory.atomic.*`. The module's own stop
flag and counters are `std::atomic` as well. The socket, pump threads, join
script state and mutex are held in a process-wide session; a second `start`
throws until `shutdown()` is called.

## Ring ABI

Absolute addresses in the guest's 2 GiB shared memory. These match
`tools/patch.py` (`RING_BASE = 0x7FF00000`) and `iwa/abi.js`, including the
**1536-byte slot stride** used by the patched engine (the Rust research port
defaulted to 1524; see *Parity notes*).

| Absolute | Offset | Size | Meaning |
| --- | --- | --- | --- |
| `0x7FF00000` | `0x00` | u32 | `MAGIC` = `0x4B52414E` (`'RAKN'`); written last by `install()` |
| `0x7FF00004` | `0x04` | u32 | `TX_HEAD` — guest writes, host reads |
| `0x7FF00008` | `0x08` | u32 | `TX_TAIL` — host writes, guest reads |
| `0x7FF0000C` | `0x0C` | u32 | `RX_HEAD` — host writes, guest reads |
| `0x7FF00010` | `0x10` | u32 | `RX_TAIL` — guest writes, host reads |
| `0x7FF00014` | `0x14` | u32 | `WAKE` futex word; guest waits, host notifies via `wake` |
| `0x7FF00018` | `0x18` | u32 | `SELECTS` counter (guest) |
| `0x7FF0001C` | `0x1C` | u32 | `RECVD` count (guest) |
| `0x7FF00020` | `0x20` | 16 B | `ADDR` sockaddr captured from guest `sendto` |
| `0x7FF00030` | `0x30` | u32 | `PEER` — `RakPeer*` observed by `UpdateNetworkLoop` |
| `0x7FF00034` | `0x34` | u32 | `UPDATES` counter (guest) |
| `0x7FF00038` | `0x38` | u32 | `CREQ` — host sets 1 to request `RakPeer::Connect` |
| `0x7FF0003C` | `0x3C` | i32 | `CRES` — result of that connect (`-12345` = pending) |
| `0x7FF00040` | `0x40` | 48 B | `HOST` — NUL-padded ASCII address from the join script |
| `0x7FF00070` | `0x70` | u32 | `PORT` — peer port from the join script |
| `0x7FF00080` | `0x80` | 64 × 1536 B | `TX0` slots |
| `0x7FF18080` | `0x18080` | 64 × 1536 B | `RX0` slots |

`install()` writes `HOST` (48 zero-padded bytes), `PORT`, `CREQ = 0`,
`CRES = -12345`, then `MAGIC` **last** as the "pump is live" flag. After both
pump threads are running, `start()` sets `CREQ = 1` (unless
`StartOptions::arm_connect` is false); the patched `select()` consumes it and
reports the result in `CRES`. A non-pending change in `CRES` is logged to
stderr.

Each slot is `u32 len` followed by the payload (`MAX_DATAGRAM = 1500` bytes).
The publisher zeroes the length, writes the payload, and stores the length
last, so zero always means "not ready". Slot index is `seq & 63`; TX is
produced by the guest, RX is produced by the host. The host advances `RX_HEAD`
only after the payload and length are visible, then invokes `wake`.

## Pump lifecycle

* **TX thread** (`rk-tx`): loads `TX_HEAD`/`TX_TAIL`, copies each ready slot,
  applies `Rupp::stamp`, `sendto`s it to the single peer, then advances
  `TX_TAIL`. Empty polls back off from 2 ms to 10 ms and snap back on activity.
  A length of 0 or > 1500 ends the current batch (the guest has not finished
  publishing).
* **RX thread** (`rk-rx`): blocking `recvfrom` with a 250 ms timeout, applies
  `Rupp::learn`, copies the datagram into the next free RX slot, advances
  `RX_HEAD`, calls `wake`, and increments the RX counter. `EAGAIN`/`EINTR`
  timeouts are ignored; a full ring drops the datagram.
* **Shutdown**: `stop()` sets a shared atomic flag; each thread exits on its
  next poll/recv tick (at most ~250 ms). `shutdown()` calls `stop()`, joins
  both threads, closes the socket and clears the started flag. Call it from a
  non-pump thread once the guest no longer reads/writes the ring. The threads
  are *not* detached, unlike the Rust research port.

`WC_RAKNET_DBG` (any value) enables verbose logging. `WC_RK_SLOT` overrides the
slot stride, parsed like the Rust port but clamped to at least
`MAX_DATAGRAM + 4` so a datagram cannot spill into the next slot; use the
default 1536 with the stock patched engine. `stats()` returns cumulative TX,
RX, dropped-RX and socket-error counts.

## RUPP rewriting

`Rupp` rewrites only three session-owned fields in place, inside the TLV block
whose big-endian length is in bytes 2–3 (framing must start `01 00`, block
length ≥ 4 and ≤ datagram length). Ported from `iwa/rupp.js`:

| Field | Match | Value written |
| --- | --- | --- |
| `NetStackTokenValue` | TLV type 1, len 17, subtype 1 | `join` token (16 bytes) |
| Reply2 session token | TLV type 1, len 17, subtype 2 | learned from inbound datagrams via `learn()` |
| RCC address/port | TLV type 2, len 6 | 4-byte `MachineAddress`, then port |

The address port is `NetStackPort` for subtype 1 (handshake) and
`ServerConnections[0].Port` for subtype 2 (established session) when that port
is known; advertising `NetStackPort` after the handshake black-holes traffic.
`learn()` never overwrites a token it already recorded.

## Join-script fields consumed

`parseJoin()` accepts either a JSON object or a JSON string containing one
(the web reply double-encodes `joinScript`). It reads exactly:

* `UdmuxEndpoints[0].Address` (or `ServerConnections[0].Address` when there is
  no UDMUX endpoint) — IP literal, IPv4 or IPv6 — and its `.Port` (1…65535).
  These become the UDP peer and the `HOST`/`PORT` ring fields.
* `MachineAddress` — IPv4 dotted quad — and `NetStackPort` (1…65535): the RCC
  address and handshake port stamped into TLV type 2.
* `NetStackTokenValue`, falling back to `TokenValue`: exactly 32 hex characters
  or base64 of exactly 16 bytes.
* `ServerConnections[0].Port`, falling back to `ServerPort`: the established
  session's data port, used for TLV type 2 subtype 2.

`Rupp::setJoinScript()` reads the same fields, and also accepts
`ServerConnections[0].Address` as a fallback RCC address. It is intentionally
best-effort and idempotent: values already learned are never overwritten.

There is no JSON dependency in the repository and system `nlohmann/json` is
not part of the host build, so `raknet.cpp` contains a small recursive-descent
JSON reader (objects, arrays, strings with `\\u` escapes, numbers, booleans,
null). Numeric fields are taken from the original token via `strtoull` so
ports stay exact; other numbers go through `double`. Malformed JSON is
rejected, not interpreted leniently.

## Parity notes, simplifications, blockers

* Faithful port of `Ring`, `Rupp`, `parse_join`, `start`, the two pump loops,
  stats and the debug/error paths, with the same constants, masking, ordering
  and 250 ms RX timeout. Thread names (`rk-tx`/`rk-rx`) are set with
  `pthread_setname_np` to match the Rust builders.
* Slot stride is 1536 as required by `tools/patch.py` and `iwa/abi.js`; the
  Rust research file's 1524 default (with a `WC_RK_SLOT` override) was a
  mismatch with that ABI. The env override and its clamp are preserved.
* Deviations: C++ exceptions instead of `Result`; a joinable session instead
  of detached threads, with explicit `shutdown()`; a `wake` callback instead
  of calling `SharedMemory::atomic_notify` directly (the C++ side owns no
  VMContext); IPv6 peers are supported; `WC_RK_SLOT` is also floored at 1504;
  malformed join scripts with an explicit host/port are tolerated.
* The RX pump copies up to 1500 bytes into a slot, while the guest's recv
  splice itself caps copies at `MAX_DATAGRAM - 8`; this matches the Rust and
  JS pumps.
* Blockers: none for compilation or local loopback verification. Full
  integration still needs the other workers' VMContext, imports and `main.cpp`
  to supply a 2 GiB mapped guest memory and a working guest notify callback.
  CREQ-driven connect assumes the engine was built with
  `CONNECT_ON_SELECT` from `tools/patch.py`. No live game session was run.
