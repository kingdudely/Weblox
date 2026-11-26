# `weblox::WebTransport` — native C++17 WebTransport over libquiche

`wt.h` / `wt.cpp` are a self-contained native-host WebTransport client. They
reproduce the handshake and keepalive behaviour proven by
`tools/quiche-probe/src/main.rs` (see `docs/QUICHE-FEASIBILITY.md` and
`docs/QUICHE-LONGEVITY.md`) using libquiche's **C API** directly — no browser,
CEF, Deno or game client is involved.

Files owned by this module: `host/wt.h`, `host/wt.cpp`, `host/WT.md`,
`host/build_quiche.sh`. No binaries are committed.

## Build and link

The crate used by the probe is quiche **0.30.1** (with BoringSSL 5.6.0). There
is no system `libquiche` on this machine, so `build_quiche.sh` builds a single
static archive from the cargo registry source into `/tmp`:

```sh
./host/build_quiche.sh
# or: QUICHE_PREFIX=/tmp/my-quiche ./host/build_quiche.sh
```

Output (nothing inside the repo):

```
/tmp/weblox-quiche/include/quiche.h
/tmp/weblox-quiche/lib/libweblox_quiche.a
/tmp/weblox-quiche/link.mk          # WEBLOX_QUICHE_CXXFLAGS / _LDFLAGS
```

Link flags:

```sh
CXXFLAGS=-std=c++17 -I. -I/tmp/weblox-quiche/include
LDFLAGS=-L/tmp/weblox-quiche/lib -lweblox_quiche -ldl -lpthread -lm
g++ $CXXFLAGS -c host/wt.cpp -o wt.o
g++ $CXXFLAGS host/main.cpp wt.o ... $LDFLAGS -o native-host
```

Use **g++** (or clang++ with the C++ runtime) for the final link: the bundled
BoringSSL objects are C++ and need `libstdc++`. The archive already contains
the Rust runtime, quiche and BoringSSL; only `-ldl -lpthread -lm` are needed.

The build script applies two mechanical patches to a copy of the crate under
`$QUICHE_PREFIX/build` (the repository copy is never modified):

1. quiche 0.30.1 gates its C FFI behind feature `ffi`, which pulls in the
   optional `cdylib-link-lines` build dependency (not in our offline cargo
   cache, and only needed for macOS cdylib link flags). The feature is made
   self-contained so `cargo build --release --offline` works and produces one
   staticlib with all `quiche_*` C symbols.
2. The Rust H3 API has `Config::set_additional_settings()`, but the C API does
   not expose it. Roblox requires the four draft-02 WebTransport SETTINGS, so
   the script adds `quiche_h3_config_set_additional_settings(config, keys,
   values, len)` to the C ABI and to the copied `quiche.h`. `wt.cpp` calls it
   and treats failure as a warning rather than a fatal error.

The first build compiles BoringSSL and takes ~6 minutes; later builds are
incremental. `QUICHE_CRATE_DIR` overrides the registry lookup.

## API

```cpp
#include "wt.h"

weblox::WebTransport wt;
wt.set_log_callback([](const std::string& line) { /* your logger */ });
wt.set_status_callback([](weblox::Status s, const std::string& msg) { /* ... */ });
wt.set_datagram_callback([](std::vector<uint8_t> payload) { /* ... */ });
wt.set_stream_callback([](const weblox::StreamMessage& m) { /* ... */ });

weblox::WebTransport::Options o;             // defaults match the probe
o.keepalive_ms = 5000;
o.idle_timeout_ms = 60000;
// host = browser/UDMUX .34 address; fqdn = qdmux game FQDN (SNI + :authority)
if (!wt.connect(host, port, fqdn, caps, cert_hash, o)) { /* last_error() */ }

// In the host loop: call wt.pump() regularly, or wt.run_for(100ms).
// wt.session_id() is the CONNECT stream id (= 0 for the first session).
wt.send_datagram({0xde, 0xad, 0xbe, 0xef});  // qdmux varint prefix added
std::vector<uint8_t> payload;
while (wt.recv_datagram(&payload)) { /* ... */ }

std::vector<weblox::StreamMessage> msgs;
wt.drain_streams(&msgs);
int64_t s = wt.open_uni_stream();              // WT_UNI header written
wt.send_stream_data(uint64_t(s), {1, 2, 3});
wt.close();
```

`connect(host, port, fqdn, caps, cert_hash)` is also available with default
options. It blocks until `:status 200` or `connect_timeout_ms` (15 s default),
then returns; drive I/O afterwards with `pump()` / `run_for()`.
`is_open()`, `status_code()`, `session_id()`, `cert_pin_verified()`,
`keepalives_sent()`, `packets_sent()`, `packets_recv()`, `stats_string()` and
`last_error()` are available for diagnostics.

### Protocol details implemented

- QUIC v1, ALPN `h3`; local settings: `max_idle_timeout=60000`,
  receive/send UDP payload 1472, active migration disabled,
  `initial_max_data=15728640`, per-stream 6291456, 100 bidi/uni streams,
  H3_DATAGRAM enabled, extended CONNECT enabled, QPACK 65536/100,
  `max_field_section_size=65536`, and draft-02 settings
  `0xffd277`, `0xc671706a`, `0x2b603743`, `0x2b603742` = 1.
- Extended CONNECT headers: `:method=CONNECT`, `:scheme=https`,
  `:authority=<fqdn>:<port>`, `:path=/wt`, `:protocol=webtransport`,
  `sec-webtransport-http3-draft02: 1`, `x-rbx-capabilities=<caps>`; wait for
  `:status 200` and expose the echoed `x-rbx-capabilities` in the log.
- Certificate pin: quiche's Config has no verification callback, so BoringSSL
  CA/chain validation is disabled. After the handshake we compute SHA-256 of
  the peer DER certificate and compare it to the route's base64
  `certHashes[0]` (the same pin Chromium checks). A mismatch aborts with an
  error. An empty hash is allowed only with
  `Options::require_cert_hash = false`, and is logged as a warning.
- Keepalive: `quiche_conn_send_ack_eliciting()` every 5 s, flushed immediately;
  `quiche_conn_timeout_as_millis()` / `quiche_conn_on_timeout()` drive loss
  detection; socket `poll()` is capped at 100 ms so the PING cadence stays
  precise. Handled as in the 240 s Rust longevity probe.
- DATAGRAMs: send prepends the qdmux session-id QUIC varint (0 → `0x00`);
  receive strips and verifies it before invoking `on_datagram` or queueing for
  `recv_datagram()`. Payload budget is checked via
  `quiche_conn_dgram_max_writable_len()`.
- Streams: server-initiated streams with first byte `0x54` (WT_UNI) / `0x41`
  (WT_BI) followed by the session-id varint are accepted and their payload
  bytes exposed through `on_stream` / `drain_streams()`. `open_uni_stream()` /
  `open_bi_stream()` write the corresponding type header on newly created
  streams and `send_stream_data()` writes bytes. H3-internal streams (client
  bidi 0, client uni 2/6/10, server uni 3/7/11) are never consumed here.

## Bounded self-test (60 s)

Compile the built-in self-test (only `wt.cpp`, no other repo files):

```sh
g++ -std=c++17 -O2 -Wall -Wextra -DWEBLOX_WT_SELFTEST -Ihost \
  -I/tmp/weblox-quiche/include -o /tmp/wt-selftest host/wt.cpp \
  -L/tmp/weblox-quiche/lib -lweblox_quiche -ldl -lpthread -lm
```

Generate a fresh route immediately before the run (routes expire within
minutes), then pass the browser `.34` host, not the native `.33` host:

```sh
PLACE=189707 python3 tools/quiche-probe/fresh_route.py > /tmp/wt-cpp-route.json
# expanded from that JSON:
timeout 120 /tmp/wt-selftest \
  --host 128.116.2.34 --port 50768 \
  --fqdn b9d5f49e499061ea0264ab181edbd673-0a25014d-c650.80740222.qdmux.roblox.com \
  --caps 120ddbc750b31896 \
  --cert-hash 'w2l+lJ7KQcT0D6VGNVswy4jW9xaYcM4xdnnxIWH24Ww=' \
  --observe-secs 60 --timeout-ms 15000 > /tmp/wt-cpp-selftest2.log 2>&1
echo $?   # 0
```

Route generated at 2026-11-26 (UTC) immediately before the run; the run
exercised QUIC handshake, cert pin, H3 SETTINGS, extended CONNECT and a 60 s
keepalive observation window. Verbatim evidence from
`/tmp/wt-cpp-selftest2.log`:

```
[wt] t=0ms ALPN=h3 local_fd=3 peer=128.116.2.34:50768 sni=b9d5...qdmux.roblox.com authority=b9d5...:50768
[WARNING] quiche Config has no verify callback: BoringSSL CA/chain validation disabled; only the SHA-256 DER hash pin is checked.
[quic] t=1ms sent initial packet (1200 bytes)
[cert] t=75ms SHA-256 DER hash matches pinned route hash (w2l+lJ7KQcT0D6VGNVswy4jW9xaYcM4xdnnxIWH24Ww=)
[h3] t=75ms local SETTINGS sent on control stream
[h3] t=75ms extended CONNECT sent on stream 0 (...:50768/wt, protocol=webtransport, draft02=1, caps=120ddbc750b31896)
[h3] t=218ms peer SETTINGS: [6=4611686018427387903, 1=0, 7=0, 51=1, 8=1, 727725890=1, 746385408=1, 3329323114=1]
[h3] t=218ms response stream=0 headers=[:status: 200, x-rbx-capabilities: 120ddbc750b31896]
[RESULT] t=218ms CONNECT :status = 200
[status] t=218ms status=2 CONNECT 200; session id 0
[observe] START t=218ms target=60s
[keepalive] t=218ms QUIC PING #1 (cadence=5000ms)
...
[keepalive] t=55218ms QUIC PING #12 (cadence=5000ms)
[observe] END t=60219ms observed_s=60.00
[summary] handshake=true h3=true connect_status=200 cert_pinned=1 survived=1 closed=0 error=
[summary] pings=12 packets_sent=159 packets_recv=208 recv=208 sent=161 lost=1 retrans=2 sent_bytes=12638 recv_bytes=15968 dgram_recv=0 dgram_sent=0
```

Peer SETTINGS echo `8` (ENABLE_CONNECT_PROTOCOL), `51` (H3_DATAGRAM) and the
draft-02 values `727725890` (`0x2b603742`), `746385408` (`0x2b603743`),
`3329323114` (`0xc671706a`), matching the Rust probe exactly. The session was
still open at the end of the window; no `[ERROR]`, `[CLOSE]` or timeout
occurred. An earlier identical 60 s run on another fresh route also passed
(`connect_status=200 survived=1`, 12 pings, 0 errors).

Scope note: `dgram_recv=0` is expected — the probe/self-test does not speak the
Roblox game protocol, so the server had no reason to send qdmux datagrams or
open data streams. It proves session establishment and liveness, not RNA
mapping.

## What works / what remains

**Works:** route-specific QUIC+H3 WebTransport handshake (`:status 200`),
certificate-pin verification, draft-02 SETTINGS negotiation, 5 s keepalive
with 60 s local idle timeout, UDP I/O, qdmux datagram framing, WT stream type
header parsing for server-initiated streams, basic client stream creation,
status/log callbacks, and clean shutdown.

**Remains for full Roblox RNA mapping:**

1. **Game protocol above WT**: decode/encode the Roblox RNA messages carried in
   the 12 data streams and datagrams; open the exact stream layout the engine
   expects. The self-test saw no server-initiated streams because no RNA was
   sent.
2. **Flow control / partial writes**: `send_stream_data()` makes one best-effort
   `quiche_conn_stream_send()` call and does not retry partial writes or honour
   writable events; a production sender needs a per-stream output queue.
3. **Stream-id policy**: client WT uni streams start at 14 (2/6/10 are H3), and
   server uni ids 3/7/11 are assumed to be H3 control/QPACK and skipped. This
   is sufficient to receive WT streams but not a complete H3/WT stream
   demultiplexer.
4. **Datagram budgets/fragmentation**: only single-frame datagrams are sent;
   no fragmentation or RNA-level sequencing.
5. **Route lifecycle**: call `fresh_route.py` (or gamejoin) when a route
   expires; dial the browser `.34` host and preserve the WARP/egress routing
   that makes the Roblox UDMUX address reachable.
6. **H3 PING / application liveness**: quiche 0.30.1 exposes no H3 PING sender;
   QUIC-level PING is what resets the idle timer and is sufficient here.
