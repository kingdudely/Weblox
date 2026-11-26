# Native join-route helper (`host/route.h`, `host/route.cpp`, `host/route_probe.cpp`)

The native host must supply the browser/qdmux WebTransport route itself: the
join reply obtained with a native identity has no `gameFqdn`, and the browser
route is the only address that accepts the WebTransport handshake. This module
does one thing: POST the join with the native identity, retry Roblox's
intermittent empty-token replies, and derive the route described in
`docs/WEB-PLAY-GATE.md` and `docs/QUICHE-FEASIBILITY.md`.

No new dependency beyond libcurl (8.14.1 on this box). JSON is read by a small
strict parser local to `route.cpp`, the same style as `host/raknet.cpp`.

## API

```cpp
#include "route.h"

weblox::JoinRoute route = weblox::fetch(placeId, cookie);
// or: weblox::JoinRoute::fetch(placeId, cookie)
// or with explicit bounds: weblox::fetch(placeId, cookie, options);
```

`weblox::JoinRoute` carries:

| Field | Source |
| --- | --- |
| `fqdn` | derived qdmux name (below) |
| `host` | browser VIP, `UdmuxEndpoints[0].Address` with last octet + 1 (e.g. `.33` → `.34`) |
| `port` | `NetStackPort` (the port used for the WT CONNECT) |
| `caps` | `NetStackConfig.caps` (`x-rbx-capabilities`) |
| `cert_hash` | `NetStackConfig.certHashes[0]` (base64 SHA-256 DER pin) |
| `pub_key` | `NetStackConfig.pubKey` (may be empty) |
| `join_script` | decoded `joinScript` as JSON text — keep this for the later RNA join |
| `netstack_config` | percent-decoded `NetStackConfig` as JSON text |
| `native_host`, `native_port` | the native `UdmuxEndpoints[0]` endpoint |
| `place_id`, `game_id`, `status` | request/reply context (`game_id` = `reply.jobId`) |

Failures throw `std::runtime_error`; no partially derived route is returned.

## Exact request

```http
POST https://gamejoin.roblox.com/v1/join-game HTTP/1.1
Content-Type: application/json
Cookie: .ROBLOSECURITY=<cookie>
User-Agent: Roblox/WinInet
rbx-user-agent: Roblox/Darwin
Origin: https://play.roblox.com
```

Body (one JSON object; all four IDs are freshly generated RFC-4122 v4 UUIDs,
lowercase, generated once per `fetch()` call and reused across retries):

```json
{
  "placeId": 189707,
  "isTeleport": false,
  "gameJoinAttemptId": "<uuid4>",
  "browserTrackerId": 0,
  "playSessionId": "<uuid4>",
  "eventId": "<uuid4>",
  "launchData": "",
  "joinAttemptOrigin": "PlayButton"
}
```

libcurl details: `CURLOPT_POST`, explicit `CURLOPT_POSTFIELDSIZE`, a
`curl_slist` for exactly the five headers above, write callback into a
`std::string`, total/connect timeouts, TLS verification left **on**, no
redirect following. HTTP status is checked (`2xx` required) and the body is
parsed even on non-2xx replies so the error message is useful.

### Retry

Roblox intermittently answers HTTP 200 with internal `status: 0`, or with
`status: 2` but an empty `NetStackTokenValue` (`client/page/xhr.ts` documents
the same behaviour). `fetch()` retries, with the same body:

1. transport error (curl), non-2xx HTTP, unparseable reply, or `status != 2`;
2. missing/empty/unparseable `joinScript`;
3. missing/empty/invalid `NetStackTokenValue`.

Defaults: `max_attempts = 5`, linear backoff `250 ms * attempt` capped at 2 s,
`timeout_ms = 30000`, `connect_timeout_ms = 10000`. The probe lowers these to
8 s / 5 s / 200 ms so a single invocation stays well under one minute.
A structurally malformed *usable-looking* reply (e.g. no `certHashes[0]`, bad
`MachineAddress`) is a hard error, not a retry: it means the reply shape
changed, not that Roblox asked us to try again.

## Derivation

`joinScript` is accepted either as a JSON object or as a JSON string containing
one. `NetStackConfig` is likewise an object or a percent-encoded JSON string;
`%XX` is decoded exactly once (no `+` → space translation, matching
`urllib.parse.unquote`).

```
token    = NetStackTokenValue, 32 hex chars -> 16 bytes
           otherwise base64 -> must decode to exactly 16 bytes
tokenHex = lowercase hex(token)                       (32 chars)
machHex  = lowercase hex of the 4 octets of MachineAddress        (8 chars)
portHex  = NetStackPort as 4 lowercase hex digits                 (e.g. f739)
native   = UdmuxEndpoints[0].Address, IPv4
browser  = native with the last octet + 1             (.33 -> .34)

fqdn = tokenHex + "-" + machHex + "-" + portHex + "." +
       hex(browser) + ".qdmux.roblox.com"
```

Only the browser `.34` address completes the WebTransport handshake; dialling
the native `.33` address fails. The last octet must not be 255. Token hex input
is supported in addition to the base64 form used by the Python reference
(`tools/quiche-probe/fresh_route.py`).

`caps`, `certHashes[0]` and `pubKey` come from the decoded `NetStackConfig`,
**not** from the outer reply. `JoinRoute::port` is `NetStackPort` (confirmed
equal to `NetStackConfig.port` in observed replies). A server-supplied
`gameFqdn` is ignored: we always derive ours from the authoritative fields.

Routes are short-lived: call `fetch()` immediately before opening the QUIC
session, never reuse a route from an earlier run, and keep the WARP/egress
routing that makes the UDMUX VIP reachable.

## Build and standalone probe

```sh
cd host
g++ -std=c++17 -O1 -g -Wall -Wextra -I. \
    -o route_probe route_probe.cpp route.cpp -lcurl
timeout 60s ./route_probe 189707                  # cookie: /home/john/mizg.txt
COOKIE_FILE=/path/to/cookie timeout 60s ./route_probe 189707 --show-script
```

`route_probe.cpp` prints one JSON object on stdout (route fields, `gameId`,
`joinScriptLength`; the full script only with `--show-script`) and timing on
stderr. It needs no wasm object, no browser and no game client.

## Evidence (2026-11-26, this worktree)

Offline cross-check against a previously captured native join reply —
`derive()` on `/tmp/opencode/join-native.json` reproduced
`/tmp/opencode/join-route.json` byte for byte in the fields that matter:

```text
9c0b7564852590961be900c99f053760-0abe0285-d8a1.80743822.qdmux.roblox.com
128.116.56.34:55457
120ddbc750b31896
/wm7sDcyj4cg944Li/6uTybuCgKErKlF5edg2cqv26c=
QRu4RC7+Q5ka/JuhdtC5MZ+dLA9WZro5MGsT87aUj+E=
```

Live probe, `timeout 60s /tmp/opencode/route_probe 189707` (exit 0, 740 ms,
one POST, no retry needed):

```text
[route-probe] place=189707 elapsed=740ms status=2 attempts<=5
{
  "fqdn": "5108525c04c718fab98b0d32a89eaa71-0a0800d5-f739.80745f22.qdmux.roblox.com",
  "host": "128.116.95.34",
  "nativeHost": "128.116.95.33",
  "port": 63289,
  "caps": "120ddbc750b31896",
  "certHashes": ["829XU1vF9R5dQ0x9TThyVuNhoN7gU5y0dv/FsNSJO5I="],
  "pubKey": "1aJteuattOLzYlAvM/UtonZUK00vygl28U0tEQmszqA=",
  "gameId": "1b856ecd-2723-4795-80bf-0b0ab4c81074",
  "placeId": 189707,
  "joinScriptLength": 4557
}
```

Raw captures: `/tmp/opencode/route_probe_run1.json`,
`/tmp/opencode/route_probe_run1.err`. The FQDN is fresh and distinct from the
one in `docs/QUICHE-FEASIBILITY.md`, which illustrates why a saved route must
not be reused.

Compile line produced no warnings with `-Wall -Wextra`.

## Error cases

| Symptom | Behaviour |
| --- | --- |
| empty cookie file / empty cookie string | immediate `std::runtime_error` before any request |
| `max_attempts == 0` | immediate `std::runtime_error` |
| curl failure / DNS / TLS / timeout | retried; final error includes `curl_easy_strerror` and curl's error buffer |
| HTTP non-2xx | retried; final error includes status and first 200 bytes of body |
| `status: 0` or any other non-2 internal status | retried; final error names the last status |
| `joinScript` absent, null, or not object/string | retried |
| `NetStackTokenValue` empty, short, bad base64/hex, or wrong length | retried |
| `NetStackConfig` missing / bad percent escape / bad JSON | hard error |
| `certHashes` empty, `caps` missing | hard error |
| `MachineAddress` or UDMUX address not IPv4, last octet 255 | hard error |
| `NetStackPort` missing, 0 or > 65535 | hard error |

Known limitation: retry policy is fixed (5 attempts); it does not read
`minimum-retry-wait-seconds` from the response headers, and the 5-attempt
budget is intentionally smaller than the extension's 30 retries. The probe
passes its own bounds so the run cannot exceed the 60 s harness limit.
