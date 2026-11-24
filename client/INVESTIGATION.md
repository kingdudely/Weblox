# Why Deno is stuck on "Joining server" — investigation record

## Symptom

The engine reaches `NetworkClient:Create` → `Connection accepted` → `serverId`
→ `Replicator created`, then never logs `Time taken to initialize schema` or
`Assigned peer id`. The GUI stays on "Joining server", then eventually shows Roblox's "lost
connection due to an unknown error" screen (confirmed by the user in the native
window). Chrome with the same engine/account/place gets schema in ~40-100 ms and
finishes loading.

A separate input bug was fixed alongside this: pointer events were delivered
only to the canvas, so buttons on that in-engine screen (retry/leave) could not
be clicked. They now bubble through `body` → `document` → `window` as they do in
a browser. This does not change the join outcome.

## Definitive finding

**The server never opens Roblox's 12 data streams for our WebTransport session.**

Instrumented Chrome's extension worker and our `env.ts` wrapper to log every
`createUnidirectionalStream()` write. The two clients send byte-identical
protocol messages:

```
12x stream header   06 01 04 <channel u32be>       (identical)
ctrl hello          80 00 00 02 00 00              (identical)
OpenUnreliable      80 00 00 0a 02 04 ...          (identical ch3/5/6/11 wires 2/4/6/8)
ctrl app=0          06 01 00 63 74 72 6c            (identical)
early-auth          80 00 00 67 a8 12 21 ... 107B  (identical structure)
0x90                12600-ish B                      (session data differs)
0x92                14 B
0x8A                1780-ish B
0x8F                6 B
```

Server response difference:
- **Chrome**: 2 server streams open *before* early-auth, 8 more after, then a
  2 MB+ continuous DataModel stream (QUIC stream 27). NetLog confirms.
- **Deno**: **zero** server streams. Only 32-byte datagrams.

Those 32-byte datagrams are **normal keepalives** — Chrome receives byte-identical
ones (`05 <seq> 00 f1 14 10 <ts1> <ts2> 00...`). They are not a rejection.

## Everything ruled out (with evidence)

| Hypothesis | Evidence against |
|---|---|
| QUIC stack / Origin / headers | 4 implementations fail identically: Deno/quinn, wtransport napi, libquiche, rwebtransport (quiche+BoringSSL, Chrome TLS profile, Origin+`sec-webtransport-http3-draft02`). Local WT server confirmed our rwebtransport options emit exactly Chrome's CONNECT headers. |
| TLS fingerprint | rwebtransport uses BoringSSL with Chrome's curve/sigalg/GREASE profile. |
| Low-level UDP framing | dumpcap on `CloudflareWARP`: both use QUIC v1, both get Retry packets, both use RUPP framing to UDMUX. Only normal stack differences (DCID 8 vs 20, TP values). |
| `getStats()` fake | `WC_REAL_STATS=1` (real zeros) → same failure. |
| Wrapper interference | `WC_WT_PASSTHROUGH=1` → same failure. |
| Account | `/tmp/opencode/jebus.txt` (jebuscristgaming, verified in 0x90 payload) → same failure. `mizg` (MissGurl_2345) → same failure. |
| HTTP User-Agent / Origin on join | `WC_JOIN_HTTP_UA` / `WC_JOIN_NO_ORIGIN` → same failure. |
| Join response structure | HAR vs our dump: both 70 keys, identical key sets; only 4 per-session value differences (`TokenValue`, `PepperId`, `AccountAge`, `CharacterAppearance` URL). Same top-level keys incl. `authenticationTicket`. |
| Engine build mismatch | Both use `version-1ef59424f70d4932-RobloxWebClient` (HAR + mirror sidecars). |
| `gameFqdn` derivation | Same formula as extension; `rawGameFqdn` always null; we now skip patching when it is present. |
| A7 message missing from writes | Chrome's writes also contain no distinct A7 message. Not the differentiator. |

## Decisive control experiment — same job, same account (PASSED)

Using `WC_JOB`, Deno can be pinned to the exact job Chrome joined:

```sh
WC_COOKIE_FILE=/tmp/opencode/jebus.txt \
WC_JOB=fc522568-72ee-448d-b9d0-bfc1639343f2 \
./run.sh 189707
```

This sends `POST /v1/join-game-instance` with `gameId` set to Chrome's job UUID.
Result: the response has the **exact same** `MachineAddress` (10.190.0.80),
`UdmuxEndpoints` (128.116.56.33:50612), and `NetStackPort` (63487) as the
`jebuscristgaming` join in the sensitive HAR — i.e. the identical server and
credentials Chrome used successfully. Deno **still** gets zero server streams
and no schema.

This definitively rules out: server/job variance, account, join-response
structure, and `gameFqdn` derivation. The discriminator is entirely on the
client side: the WebTransport implementation or the wasm's environment.

## Flow-control hypothesis (being tested)

QUIC transport parameters reported by Chrome's NetLog vs the stacks we tried:

| parameter | Chrome | Deno/quinn | rwebtransport (before patch) |
|---|---|---|---|
| `initial_max_stream_data` (uni) | **6,291,456** | ~1.25 MB | 1,048,576 |
| `initial_max_data` | 15,728,640 | — | 10,485,760 |
| `max_udp_payload_size` | 1472 | — | 1350 |
| `max_streams_uni` | 103 | 100 | 256 |
| `max_datagram_frame_size` | 65536 | — | 65536 (quiche `enable_dgram`) |

Roblox's DataModel stream is **2 MB+**. A 1 MB / 1.25 MB per-stream receive
window cannot accept it: the server opens the stream but is immediately
flow-control blocked, so no STREAM frame bytes ever arrive and the client never
observes an incoming stream. This would explain why *all four* stacks fail
identically and why the server keeps pinging instead of closing the session.
rwebtransport's Rust source (`crates/native/src/config.rs`) has these values as
constants; Chrome's H3 SETTINGS from the NetLog are
`ENABLE_CONNECT_PROTOCOL=1, H3_DATAGRAM=1, MAX_FIELD_SECTION_SIZE=65536,
QPACK_BLOCKED_STREAMS=100, QPACK_MAX_TABLE_CAPACITY=65536` plus GREASE — no
Roblox-specific WebTransport setting.

An rwebtransport rebuild with Chrome's values was completed and tested:
**still no streams.** A second rebuild with Chrome's exact H3 SETTINGS (only
`ENABLE_CONNECT_PROTOCOL`, `H3_DATAGRAM`, `MAX_FIELD_SECTION_SIZE`, QPACK — no
WebTransport session-credit settings) also failed. The stock addon/source were
restored afterwards. Flow control and H3 SETTINGS are now ruled out.

## Final byte-level comparison of the 0x90 client-info message

With the job pinned to Chrome's exact job and the same account, Chrome's and our
0x90 payloads (from full WebTransport write dumps; Chrome captured via an
instrumented copy of the extension + CDP) have **identical structure**: 51 keys
each, same key set apart from the randomized `rbx*` experiment-hash keys, and
all join/session fields match. The only non-session text difference is
`UserAgentStr`:

- Chrome: `Browser/Chrome/157.0.8103.78`, screen `1366x768`
- Ours: `Browser/Chromium/157.0.0.0`, screen `800x600`

Adding `WC_CHROME_HINTS=1` to replicate Chrome's Client Hints (`Google Chrome`
brand, `157.0.8103.78`, 1366x768 screen) made those strings match — and the
join **still failed**. `UserAgentStr` is not the gate.

Also verified: the wasm really does call `incomingUnidirectionalStreams.getReader()`
(`[wt ] GET READER ...`), but `read()` never resolves with any value — no server
stream reaches the JS layer. Incoming datagrams are only 32/33-byte keepalives
(2033 + 1933 observed); there is no larger challenge datagram.

## Clean WARP capture + stream-header logging (latest)

A clean capture with Chrome fully closed and filtered to our exact qdmux
4-tuple showed the server sending **1.4 MB / 1005 large packets** to Deno — so
data does arrive at the QUIC layer. An instrumented rwebtransport build that
logs every classified incoming stream header (`[DBGWT]`) logged **zero** WebTransport
streams: the server never opens them at the H3 layer for our session.

Ruled out after that: the WebTransport stream session-ID check (patched to
accept any value), Chrome's full CONNECT header set (`WC_WT_FULL_HDRS=1`:
`sec-fetch-*`, `sec-ch-ua-*`, Origin, draft02, UA), and a real numeric
`RBXEventTrackerV2` browserid (Chrome's `BrowserTrackerId` is otherwise 0 for
us; `WC_BROWSER_ID=1` sets it). None changed the outcome. The stock rwebtransport
addon/source were restored.

Five WebTransport implementations now fail identically: Deno/quinn,
@webtransport-bun, @fails-components (libquiche), rwebtransport (quiche +
BoringSSL, with Chrome's TPs and H3 SETTINGS), and quico (pure JS). Chrome's
own network stack joins the same job successfully.

## What is NOT known

The discriminator is inside the join session credentials or a wasm-side state
difference that none of the above instrumentation exposes. The server accepts
the QUIC session from both clients, pings both, but only opens Roblox's game
channels for Chrome.

## Remaining hypotheses / next experiments

1. **Wasm import bisection** (user's suggestion — IN PROGRESS, inconclusive).
   The wasm imports 342 functions (`env` + `wasi_snapshot_preview1`). Stub
   groups of non-WebTransport imports and see if the join changes. Prime
   suspects: `read_host_info_int/string` (f.hostInfo — OS/GPU/browser values
   sent to gamejoin), `js_fetch_file` (asset/DataModel loading), `rbx_opfs_*`,
   `js_web_client_ms_since_navigation`, `js_web_client_page_visible`,
   `random_get`. The import object is built in `WebClient.js` as
   `{env: We, wasi_snapshot_preview1: We}`; the names are listed in `vn()`
   around `presentBrowserFrame:Ya,proc_exit:Sr`.

   A hook was added: `env.ts` reads `WC_STUB_IMPORTS` (a JSON array of import
   names) and sets `globalThis.__STUB_IMPORTS`; the **mirror** copy of
   `WebClient.js` (gitignored, regenerated from the CDN) must also carry this
   snippet inside `vn()`, right before `return e`:

   ```js
   if (globalThis.__STUB_IMPORTS) {
     for (const k of globalThis.__STUB_IMPORTS) {
       if (k in e.env) e.env[k] = function(){ return 0 };
       if (k in e.wasi_snapshot_preview1) e.wasi_snapshot_preview1[k] = function(){ return 0 };
     }
     console.error("[stub] imports", JSON.stringify(globalThis.__STUB_IMPORTS));
   }
   ```

   Re-apply that patch to `client/mirror/files/WebClient.js` after any mirror
   refresh, then rebuild. First test stubbing
   `read_host_info_int/read_host_info_string/js_web_client_page_visible/
   js_web_client_ms_since_navigation` did **not** reach WebTransport (early
   init stalled at `[pthread] thread #1 created`), so it neither confirms nor
   rules out the hypothesis — retry with a smaller group.
2. **Decompile the wasm's join-config path** to find which field decides to open
   channels (no logging flag exists in the 85 wasm exports; no logging FFlag
   string exists in the binary).
3. **Rebuild Deno's QUIC/HTTP3 TP+SETTINGS to Chrome's exact values** —
   previously rejected by the user as too heavy.
4. **Browser/CEF backend** — proven to work, loses low-RAM goal.

## Useful commands

```sh
# short A/B run
FAST_RUNS=4 /tmp/opencode/fasttest.sh env WC_WATCHDOG_MS=0

# one diagnostic run with full RNA + byte dumps
cd /home/john/Alcohol/client
WC_RNA_LOG=1 WC_RNA_DUMP=1 WC_WT_DUMP=1 WC_WATCHDOG_MS=90000 \
  WC_WT_HANDSHAKE_MS=0 WC_REJOIN=0 timeout 90 ./run.sh 189707

# parse a pcapng capture
python3 /tmp/opencode/pcapng.py /tmp/opencode/deno_warp.pcap

# Chrome with CDP (extension must be loaded in the profile)
DISPLAY=:0 google-chrome --remote-debugging-port=9337 \
  --user-data-dir=/tmp/opencode/chromeprof \
  'https://play.roblox.com/games/start?placeId=189707'
cd /tmp/opencode && deno run -A cdp_all.ts
```

Note: ~50% of raw runs stall in early init at `[pthread] thread #1 created`
before even reaching gamejoin. This is a separate flaky symptom; retry.
