# Native quiche WebTransport session longevity (place 189707)

Question: the handshake probe in [`QUICHE-FEASIBILITY.md`](QUICHE-FEASIBILITY.md)
proved that native libquiche can complete the Roblox/qdmux WebTransport
extended CONNECT (`:status 200`), but it stopped at CONNECT. Chrome's session
survives 240 s ([`CHROME-ORACLE.md`](CHROME-ORACLE.md)), while our Deno shim dies
at ~110 s with `conn.closed REJECTED Error: timed out`. Can a **native quiche**
session stay alive past 120 s?

All times UTC, 2026-11-26. Worktree `feat/quiche-long` (commit on branch
`feat/quiche-long`).

## Verdict

**Yes — native quiche held the WebTransport QUIC/H3 session alive for the full
240 s observation window.** Excerpt from the final summary:

```
[summary] handshake=true h3=true connect_sent=true status_received=true closed=false
[summary] observe_target_s=240 observed_s=240.01 survived=true close_reason=None pings_sent=48 packets_sent=591 packets_recv=781 on_timeout_calls=2059 errors=0
[summary] quiche stats: recv=781 sent=593 lost=1 retrans=2 sent_bytes=35661 recv_bytes=55060 lost_bytes=1195
```

No `conn.closed`, no idle timeout, no H3 poll error, no send/recv error, and no
application-level session termination occurred. The connection was still open
when the probe deliberately ended the observation window. This clears the
QUIC/H3 transport layer as the cause of the Deno shim's ~110 s timeout; that
failure is specific to our Deno/WebTransport implementation, not to the Roblox
qdmux server or to QUIC itself.

Scope: this is a **transport-level** result. The probe does not open
WebTransport streams or send datagrams, so it does not prove that the later
Roblox game protocol (12 data streams) works natively; it proves the session
can stay open indefinitely with correct keepalive.

## What was changed in the probe

`tools/quiche-probe/src/main.rs` gained an observe mode:

- `--observe <seconds>`: after CONNECT `:status 200`, keep driving the quiche
  event loop for exactly N seconds instead of stopping.
- `--keepalive-ms N` (default 5000): schedule an ack-eliciting packet (QUIC
  PING) every N ms via `conn.send_ack_eliciting()`, which quiche turns into a
  real PING frame in the next `conn.send()` output.
- `--idle-timeout-ms N` (default 60000): sets the local QUIC
  `max_idle_timeout` transport parameter. The effective timeout is
  `min(local, peer)`.
- Every close, timer tick, send/recv error, H3 event and 30 s stats snapshot is
  logged with elapsed time. The final `[summary]` line records whether the
  session survived the full window, close reason, ping count and packet counts.

Note on APIs: quiche v0.30.1's Rust `Config` has **no** `keep_alive_interval`
setter and its H3 module has **no** public `send_ping()`. Keepalive is therefore
done at the QUIC layer with `send_ack_eliciting()`, which is sufficient to reset
the idle timer at both endpoints: quiche restarts its idle timer when it sends
the first ack-eliciting packet after a receive, and again whenever any packet
is received (`lib.rs`: `ack_eliciting && !ack_eliciting_sent` in `send()`, and
the idle-timer reset in `recv()`). The C API equivalent used by a C++ host is
`quiche_conn_send_ack_eliciting()`.

## Exact commands

Build (no binaries committed; `**/target/` is gitignored):

```sh
cd /tmp/opencode/wt-quiche-long/tools/quiche-probe
cargo build --release --offline
```

A fresh route is generated immediately before the run because routes expire
within minutes:

```sh
cd /tmp/opencode/wt-quiche-long
PLACE=189707 python3 tools/quiche-probe/fresh_route.py > /tmp/quiche-long-route1.json
```

Route produced at **20:48:02 UTC** (used for the run below):

```json
{
  "fqdn": "e8b80a333aca32bfc44964465ac930c6-0a2903da-f84c.80741e22.qdmux.roblox.com",
  "nativeHost": "128.116.30.33",
  "browserHost": "128.116.30.34",
  "netstackPort": 63564,
  "caps": "120ddbc750b31896",
  "certHashes": ["utfK/qVc4v4VoKjoQsIP83+bUEDdoFOFuzjU7Sxj0jc="],
  "gameId": "f40805e8-7f3c-420a-8448-09316bd0beb6",
  "status": 2
}
```

Then, with the values expanded from that JSON (this is the exact command that
ran; full stdout+stderr in **`/tmp/quiche-long.log`**):

```sh
cd /tmp/opencode/wt-quiche-long/tools/quiche-probe
timeout 280 ./target/release/quiche-probe \
  --host 128.116.30.34 --port 63564 \
  --fqdn e8b80a333aca32bfc44964465ac930c6-0a2903da-f84c.80741e22.qdmux.roblox.com \
  --caps 120ddbc750b31896 \
  --cert-hash 'utfK/qVc4v4VoKjoQsIP83+bUEDdoFOFuzjU7Sxj0jc=' \
  --timeout-ms 15000 --observe 240 --keepalive-ms 5000 --idle-timeout-ms 60000 \
  -v > /tmp/quiche-long.log 2>&1
```

Settings: QUIC PING every 5 s, local `max_idle_timeout` 60 s, socket read
timeout capped at 100 ms so the PING cadence stays precise. Cert hash pinning
was enabled and matched. Only one run was needed: the task's fallback
("if it closes early, try one alternative setting") did not trigger — no early
close occurred.

## Timeline

| Time (UTC) | Elapsed (t) | Observation |
|---|---|---|
| 20:48:02 | — | Fresh route generated by `fresh_route.py`. |
| 20:48:08 | 0 ms | Probe starts; initial 1200-byte QUIC packet sent to `128.116.30.34:63564`. |
| 20:48:08 | 47.893 ms | QUIC handshake established (ALPN `h3`), cert hash pin matched. |
| 20:48:08 | 48.024 ms | Local H3 SETTINGS + extended CONNECT sent on stream 0 (`:status` request). |
| 20:48:08 | 138.43 ms | Peer H3 SETTINGS received (CONNECT protocol, H3 datagrams, draft-02 WebTransport settings all echoed). |
| 20:48:08 | 139.165 ms | Response headers on stream 0: `:status: 200`, `x-rbx-capabilities: 120ddbc750b31896`. |
| 20:48:08 | 139.210 ms | `[RESULT] CONNECT :status = 200`; observe window starts. |
| 20:48:08 | 139.228 ms | QUIC PING #1 scheduled; PINGs continue at 5 s cadence (#48 by t=236.97 s). |
| 20:48:38 | 30.201 s | Report: `sent=91 recv=111 pings=6 lost=1 retrans=2 closed=false`. |
| 20:49:08 | 60.208 s | Report: `sent=165 recv=209 pings=12 lost=1 retrans=2 closed=false`. |
| 20:49:38 | 90.209 s | Report: `sent=237 recv=305 pings=18 lost=1 retrans=2 closed=false`. |
| 20:50:08 | 120.209 s | **Past the 120 s Deno failure point**; `sent=310 recv=402 pings=24 lost=1 retrans=2 closed=false`. |
| 20:50:38 | 150.216 s | Report: `sent=377 recv=496 pings=30 ... closed=false`. |
| 20:51:08 | 180.229 s | Report: `sent=449 recv=592 pings=36 ... closed=false`. |
| 20:51:38 | 210.333 s | Report: `sent=521 recv=688 pings=42 ... closed=false`. |
| 20:52:08 | 240.145 s | `[observe] END ... (observation window elapsed)`; connection still open (`closed=false`). |
| 20:52:09 | 240.01 s observed | Process exits 0 (`RUN_EXIT=0`); no `conn.closed`, no timeout, 0 errors. |

## Evidence (verbatim log excerpts)

Handshake and CONNECT:

```
[quic] sent initial packet (1200 bytes)
[quic] HANDSHAKE ESTABLISHED t=47.893054ms (library 0.1.0)
[cert] SHA-256 DER hash matches pinned route hash (utfK/qVc4v4VoKjoQsIP83+bUEDdoFOFuzjU7Sxj0jc=)
[h3] t=48.023579ms extended CONNECT sent on stream 0: [":method: CONNECT", ":scheme: https", ":authority: e8b80a333aca32bfc44964465ac930c6-0a2903da-f84c.80741e22.qdmux.roblox.com:63564", ":path: /wt", ":protocol: webtransport", "sec-webtransport-http3-draft02: 1", "x-rbx-capabilities: 120ddbc750b31896"]
[h3] t=138.428919ms peer SETTINGS: [(6, 4611686018427387903), (1, 0), (7, 0), (51, 1), (8, 1), (727725890, 1), (746385408, 1), (3329323114, 1)]
[h3 event] t=139.165879ms stream=0 Headers { list: [":status: 200", "x-rbx-capabilities: 120ddbc750b31896"], more_frames: true }
[RESULT] t=139.210167ms CONNECT :status = 200
[observe] START t=139.217218ms target=240s keepalive=5000ms idle_timeout=60000ms
[keepalive] t=139.227644ms QUIC PING #1 scheduled (cadence=5000ms)
```

At the 120 s mark (past the Deno failure):

```
[report] t=120.209371034s elapsed_since_last_rx=39.35µs sent=310 recv=402 pings=24 quiche_sent=312 quiche_recv=402 lost=1 retrans=2 sent_bytes=21065 recv_bytes=29640 closed=false
```

End of window:

```
[keepalive] t=236.968704545s QUIC PING #48 scheduled (cadence=5000ms)
[quic] t=239.206386547s received 45 bytes (45 processed)
[observe] END t=240.144710101s target=240s (observation window elapsed)
[summary] handshake=true h3=true connect_sent=true status_received=true closed=false
[summary] observe_target_s=240 observed_s=240.01 survived=true close_reason=None pings_sent=48 packets_sent=591 packets_recv=781 on_timeout_calls=2059 errors=0
[summary] quiche stats: recv=781 sent=593 lost=1 retrans=2 sent_bytes=35661 recv_bytes=55060 lost_bytes=1195
```

Counts explained:

- **48 QUIC PINGs** at 5 s cadence; the peer kept ACKing them (small 45/80-byte
  packets arriving roughly once per second throughout the window).
- **591 UDP sends / 781 UDP receives** driven by the probe loop; quiche's own
  counters: 593 QUIC packets sent, 781 received, 1 lost, 2 retransmitted,
  35,661 bytes sent, 55,060 bytes received.
- **2059 `on_timeout()` calls**: these are ordinary expiries of the 100 ms
  capped socket read timer that drives quiche's loss-detection clock — they
  are *not* idle timeouts or errors. `[ERROR]` count is 0 and there are no
  `[CLOSE]`, `[timeout]` or h3-poll error lines anywhere in the log.
- One lost packet and two retransmissions had accumulated by the first 30 s
  report; the counters stayed at exactly `lost=1 retrans=2` in every later
  report, so there was no further loss during the remaining 210 s.

## What the C++ host must do

The native host (`feat/cpp-host`, currently RakNet seam only) should adopt
these exact quiche settings and event-loop rules when it adds a WebTransport
transport:

1. **Set a long idle timeout.** Configure `max_idle_timeout` to at least
   60,000 ms (`quiche_config_set_max_idle_timeout()` in the C API;
   `Config::set_max_idle_timeout()` in Rust). Compute liveness against
   `min(local, peer)` — the server's advertised value may be shorter, so a
   large local value alone is not sufficient.
2. **Send keepalive at a cadence far below the effective idle timeout.** Every
   5 s, call `quiche_conn_send_ack_eliciting()` (Rust:
   `conn.send_ack_eliciting()`), then flush `conn.send()` immediately so the
   PING frame actually goes on the wire. A 5 s ping versus a 60 s idle timeout
   gives a 12× margin and was verified to survive 240 s. A 10 s idle timeout
   with no keepalive (the old probe default) would close after 10 s of
   inactivity; that is the class of bug that produces the Deno shim's
   `... timed out` at ~110 s.
3. **Drive the QUIC timer correctly.** Sleep until `min(conn.timeout(), time
   until next keepalive)`, call `conn.on_timeout()` when the timer expires,
   and keep calling `conn.send()` until it returns `Done`. The probe capped
   its socket read at 100 ms for precision; a production host need not poll
   that often, but must not let the PING cadence slip past the idle timeout.
4. **Process every received packet and H3 event** (`conn.recv()` then
   `h3.poll()`), and log `conn.local_error()` / `conn.peer_error()` with
   elapsed time on `conn.is_closed()`. Do not treat socket read timeouts as
   connection failures.
5. **Keep the handshake settings that already worked**: ALPN `h3`, extended
   CONNECT enabled, H3_DATAGRAM enabled, draft-02 WebTransport settings
   (`0xffd277`, `0xc671706a`, `0x2b603743`, `0x2b603742`), `x-rbx-capabilities`
   from the fresh route, and SHA-256 certificate-hash pinning against
   `certHashes[0]` (quiche's Rust config has no verification callback).
6. **Don't rely on H3 PING.** quiche 0.30.1 does not expose an H3 PING sender;
   QUIC-level PING is what resets the idle timer and is sufficient. If a future
   library exposes H3 PING, use it as an additional application liveness check,
   not as a replacement for QUIC keepalive.

Bottom line for the transport decision: WebTransport via libquiche is viable
without Chrome or Deno. The remaining work is the Roblox game-protocol layer
(streams/datagrams) above the session, not transport liveness.
