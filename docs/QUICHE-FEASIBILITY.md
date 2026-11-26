# libquiche feasibility: native Roblox/qdmux WebTransport handshake

Date: 2026-11-26 (UTC). Worktree: `feat/quiche`. Probe: `tools/quiche-probe/`.

## Verdict

**Yes — native libquiche can establish a Roblox/qdmux WebTransport session at
the HTTP/3 layer.** Using the Cloudflare-maintained Rust `quiche` crate
(v0.30.1, which builds/uses BoringSSL), the probe completed:

1. QUIC v1 handshake with ALPN `h3` to the browser/qdmux `.34` address;
2. local H3 SETTINGS exchange (including extended CONNECT and WebTransport
   draft-02 settings);
3. extended CONNECT to `https://<gameFqdn>:<port>/wt` with the draft-02 offer
   header and `x-rbx-capabilities`;
4. **HTTP/3 response `:status 200`**, with `x-rbx-capabilities` echoed.

This is the exact protocol handshake Chromium performs. It was reproduced with
no browser, no game client, and no Deno — only a UDP socket and `libquiche`.

Scope limit: this probe deliberately stops after the CONNECT response. It does
**not** implement WebTransport streams/datagrams or the Roblox game protocol on
top of the session (see "What remains" below).

## Acquiring a build

Standalone `google/quiche` via bazel was not attempted (bazel is not installed;
the task says not to spend time on it). The crates.io Rust crate was used:

```sh
cd tools/quiche-probe
cargo build --release
```

Observed build output (abridged):

```
 Adding base64 v0.22.1
 Adding sha2 v0.10.9
   Compiling quiche v0.30.1
   Compiling boring-sys v5.6.0
   Compiling boring v5.6.0
   Compiling quiche-probe v0.1.0
    Finished `release` profile [optimized] target(s)
```

Toolchain: `rustc 1.98.1`, `cargo 1.98.1`. No large binary is committed;
`**/target/` is already gitignored.

## Route

The supplied route in `/tmp/opencode/join-route.json` had **expired** by the
time of the test. Probing it produced no reply at all:

```
[quic] sent initial packet (1200 bytes)
[quic] connection closed by peer: client=None peer=None
[summary] handshake=false h3=false connect_sent=false status_received=false closed=true
[summary] quiche stats: recv=0 sent=7 lost=0 retrans=5 sent_bytes=6231 recv_bytes=0
```

`ip route get 128.116.56.34` shows the traffic must go via `CloudflareWARP`
(src `172.16.0.2`). A bare UDP test also timed out, i.e. the server no longer
had a listener on the old port.

A fresh route was generated with `tools/quiche-probe/fresh_route.py`:

```sh
PLACE=189707 python3 tools/quiche-probe/fresh_route.py > /tmp/opencode/quiche-fresh-route.json
```

It POSTs to `https://gamejoin.roblox.com/v1/join-game` with
`User-Agent: Roblox/WinInet`, `rbx-user-agent: Roblox/Darwin`, cookie from
`/home/john/mizg.txt`, and derives the FQDN as in `join-route.json`:
`hex(NetStackTokenValue)-hex(MachineAddress)-hex(NetStackPort).(UDMUX last octet + 1).qdmux.roblox.com`.

Fresh route used (captured in `/tmp/opencode/quiche-fresh-route.json`):

```json
{
  "fqdn": "2814ecd268f730af5d203f5c76337777-0abe001d-fe0b.80743822.qdmux.roblox.com",
  "nativeHost": "128.116.56.33",
  "browserHost": "128.116.56.34",
  "netstackPort": 65035,
  "caps": "120ddbc750b31896",
  "certHashes": ["1yhOHKRKIH/y0x4IK/z+wTHHAHsnDFTCfE5v96v5obc="]
}
```

## Exact command

```sh
cd tools/quiche-probe
./target/release/quiche-probe \
  --host 128.116.56.34 --port 65035 \
  --fqdn 2814ecd268f730af5d203f5c76337777-0abe001d-fe0b.80743822.qdmux.roblox.com \
  --caps 120ddbc750b31896 \
  --cert-hash '1yhOHKRKIH/y0x4IK/z+wTHHAHsnDFTCfE5v96v5obc=' \
  --timeout-ms 15000 -v
```

Full stdout/stderr: `/tmp/opencode/quiche-probe-run2.log`.

## Evidence (verbatim excerpts)

```
[quiche-probe] version=0.1.0 ALPN=h3 local=0.0.0.0:54300 peer=128.116.56.34:65035 sni=... authority=...:65035
[WARNING] quiche Config exposes no verify callback: BoringSSL CA/chain validation is disabled; this probe verifies only the SHA-256 DER hash against the route's pinned certHashes.
[quic] sent initial packet (1200 bytes)
[quic] received 125 bytes (125 processed)
[quic] received 125 bytes (125 processed)
[quic] received 1200 bytes (1200 processed)
[quic] HANDSHAKE ESTABLISHED after 55.14845ms (library 0.1.0)
[cert] SHA-256 DER hash matches pinned route hash (1yhOHKRKIH/y0x4IK/z+wTHHAHsnDFTCfE5v96v5obc=)
[h3] local SETTINGS sent on control stream
[h3] extended CONNECT sent on stream 0: [":method: CONNECT", ":scheme: https", ":authority: ...:65035", ":path: /wt", ":protocol: webtransport", "sec-webtransport-http3-draft02: 1", "x-rbx-capabilities: 120ddbc750b31896"]
[quic] received 678 bytes (678 processed)
[h3] peer SETTINGS: [(6, 4611686018427387903), (1, 0), (7, 0), (51, 1), (8, 1), (727725890, 1), (746385408, 1), (3329323114, 1)]
[h3 event] stream=0 Headers { list: [":status: 200", "x-rbx-capabilities: 120ddbc750b31896"], more_frames: true }
[h3 response] stream=0 headers=[(":status", "200"), ("x-rbx-capabilities", "120ddbc750b31896")]
[RESULT] CONNECT :status = 200
[summary] handshake=true h3=true connect_sent=true status_received=true closed=false
[summary] quiche stats: recv=8 sent=16 lost=1 retrans=2 sent_bytes=5747 recv_bytes=2128 lost_bytes=1195
```

Notes on the evidence:

- **QUIC handshake**: established in ~55 ms; 2 short initial replies then the
  server's 1200-byte Handshake packet. One packet was lost and retransmitted;
  quiche recovered normally.
- **H3 SETTINGS**: local SETTINGS were `max_field_section_size=65536`,
  `qpack_max_table_capacity=65536`, `qpack_blocked_streams=100`,
  `enable_connect_protocol=1`, `h3_datagram=1` (from `enable_dgram`), plus the
  four draft-02 WebTransport settings `0xffd277`, `0xc671706a`, `0x2b603743`,
  `0x2b603742`, all set to 1. Peer SETTINGS echoed `8` (ENABLE_CONNECT_PROTOCOL),
  `51` (H3_DATAGRAM), `727725890`=`0x2b603742`, `746385408`=`0x2b603743`,
  `3329323114`=`0xc671706a` — i.e. the server accepts the draft-02 offer.
- **Certificate**: quiche's Rust `Config` exposes no verification callback, so
  BoringSSL CA/chain validation was disabled (clearly labelled in the probe
  output). To avoid an unauthenticated handshake, the probe calls
  `conn.peer_cert()` after the handshake, computes SHA-256 of the DER bytes and
  compares it with `certHashes[0]` from the route. It matched exactly. This is
  the same SHA-256 pin Chromium checks via `serverCertificateHashes`.
- **Proximity header**: contrary to the task's expectation, the server did
  **not** echo a `sec-webtransport-http3-draft` (or `-draft02`) header. The
  response contained exactly two headers: `:status: 200` and
  `x-rbx-capabilities: 120ddbc750b31896`. Prior Deno draft-02 captures in
  `/tmp/opencode/*.log` show the same two-header response, so this is the
  server's normal behaviour, not a quiche artefact.

## Verdict detail

Native libquiche is sufficient for the WebTransport **handshake**. No browser
TLS stack, no CEF, and no Deno are needed. The `.34` browser multiplexer and
the pinned certificate hash are the two route-specific inputs; both worked.

This does not contradict `client/INVESTIGATION.md`: that record (now marked
outdated) concerns a later-stage failure — the server not opening Roblox's 12
data streams for non-Chrome clients. This probe establishes only that the
QUIC/H3/CONNECT gate is open to libquiche; the later stage must be tested
separately when implementing the C++ host.

## What remains for the C++ AOT host

1. **C API wiring**: `quiche_config_new(PROTOCOL_VERSION)`,
   `quiche_config_set_application_protos(cfg, b"\x02h3")`,
   `quiche_config_verify_peer(false)`, Chrome-sized transport parameters as in
   the probe, `quiche_config_enable_dgram(...)`, then `quiche_connect()` with
   SNI = game FQDN and peer = browser `.34` address. Link the BoringSSL static
   libs built by the quiche crate (or build standalone quiche with cmake +
   BoringSSL). Keep the cert-hash pin: call `quiche_conn_peer_cert()` after
   `quiche_conn_is_established()` and SHA-256 it against `certHashes`.
2. **H3 event loop in C**: `quiche_h3_config_new()` +
   `quiche_h3_config_enable_extended_connect()` +
   `quiche_h3_config_set_additional_settings()` for the four draft-02 settings,
   `quiche_h3_conn_new_with_transport()` once QUIC is established,
   `quiche_h3_send_request()` with the seven extended-CONNECT headers, and a
   drain loop over `quiche_h3_poll()` translating `quiche_h3_event_headers()`
   into `:status` / header fields.
3. **WebTransport session mapping**: session id = the CONNECT request stream id
   returned by `send_request` (0 for the first session); unidirectional streams
   are `0x54 <session-id-varint>`, bidirectional `0x41 <session-id-varint>`;
   QUIC datagrams are prefixed with `<session-id-varint>`; datagram payload
   budget = negotiated max datagram frame size minus that varint length.
4. **Long-lived session**: keepalive interval, idle timeout, stream flow
   control (the server's DataModel stream is >2 MB), and the 12 client streams
   used by the engine. Those belong to the product implementation, not to this
   feasibility probe.
5. **Route lifecycle**: refresh gamejoin when a route expires (helper script
   above), always dial the browser `.34` host, and preserve the WARP/egress
   routing that makes Roblox's UDMUX address reachable.
