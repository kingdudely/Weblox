# Rust WebTransport backend (`v2-wasmtime/src/wt.rs`)

Date: 2026-11-26 (UTC). Branch/worktree: `rust-wt`.

This records the audit of the quinn + shiguredo_http3 WebTransport backend
against the two proven references:

- `client/page/draftwt.ts` (Deno raw-QUIC draft-02 client, verified in Chrome
  NetLog terms), and
- `docs/QUICHE-FEASIBILITY.md` / `docs/QUICHE-LONGEVITY.md` (native libquiche
  probe: QUIC handshake, H3 SETTINGS, extended CONNECT, `:status 200`, 240 s
  keepalive survival).

Nothing in this change patches wasm bytes or embeds a guest pointer or a
route-specific value. Route values come from the gamejoin reply and the URL
the engine itself passes to `rbx_wasm_rna_connect_js`.

## What was audited

| Item | Proven behaviour | Rust backend before | Now |
| --- | --- | --- | --- |
| ALPN / QUIC | `h3`, QUIC v1, keepalive 4 s, idle 10 s in `draftwt.ts` (probe uses 5 s / 60 s) | ALPN `h3`, **no keepalive or idle config** | `quinn::TransportConfig::keep_alive_interval(4 s)` + `max_idle_timeout(10 s)` |
| H3 SETTINGS | `enable_connect_protocol`, `h3_datagram`, 65536 field section, QPACK 65536/100, four draft-02 WT ids | only `enable_webtransport_client` + draft-02 | added `enable_connect_protocol(true)` and the same field/QPACK values |
| CONNECT headers | `:method CONNECT`, `:protocol webtransport`, `:scheme https`, `:authority <fqdn>:<port>`, `:path /wt?…`, `sec-webtransport-http3-draft02: 1`, `x-rbx-capabilities: <caps>` | draft-02 header present; **`:authority` lacked port; `x-rbx-capabilities` absent** | authority is `host:port`; caps taken from `JOIN_SCRIPT`/guest/URL query |
| Cert pin | SHA-256 DER base64 from `certHashes` (Chrome `serverCertificateHashes`) | `NoVerify` only | `PinVerifier` SHA-256-compares peer DER with route hashes when provided; falls back to `NoVerify` only when no hashes are available |
| Route | dial browser/UDMUX `.34`; qdmux FQDN is SNI/authority only | resolved the FQDN via DNS | `ConnectOptions.peer` overrides DNS; `sni` and `authority` come from the URL/joinScript |
| Streams | `0x54 <sid-varint>` uni, `0x41 <sid-varint>` bidi | header written per `write_all` (one QUIC stream per chunk) | one QUIC stream per logical stream; header written once |
| Incoming streams | strip type + sid varint across reads; ignore foreign sessions | stripped only the first read's leading byte | incremental varint parsing with session check |
| Datagrams | prefix `<sid-varint>`, budget = max frame − prefix length | sid filter fragile; full frame size advertised | exact prefix match/strip; `max_datagram_size` subtracts the prefix |
| Session id | CONNECT stream's QUIC id | assumed 0 | taken from `SessionEstablished`; also exposed via `Connection::session_id()` |
| CONNECT result | `:status 200` | ignored timeout/status | `:status` captured, non-2xx rejected, exposed via `Connection::connect_status()` |

## API contract

```rust
let cfg = ClientConfig::builder()
    .with_bind_default()
    .with_no_cert_validation()          // compatibility name
    .with_cert_hashes(cert_hashes)      // base64 SHA-256 DER from joinScript
    .build();
let ep = Endpoint::client(cfg)?;
let opts = ConnectOptions {
    peer: Some(browser_udmux_34_socket_addr),
    sni: Some(game_fqdn.clone()),
    authority: Some(format!("{game_fqdn}:{port}")),
    caps: Some(caps),
    cert_hashes: vec![],                // pins live on ClientConfig
};
let conn = ep.connect_with(&engine_url, opts).await?;
assert_eq!(conn.connect_status(), Some(200));
```

`Endpoint::connect(url)` still works (DNS resolution, no caps) for older
callers. `rna.rs` uses `connect_with` and derives the options from:

1. the URL the engine passes (`https://<fqdn>:<port>/wt?X%2DRbx%2DCapabilities=…`),
2. `crate::imports::http::JOIN_SCRIPT` (`NetStackConfig.caps`,
   `NetStackConfig.certHashes`, `UdmuxEndpoints[0].Address` with last octet + 1,
   `NetStackPort`), and
3. the guest `certs/ncerts` arguments of `connect_js` as a fallback.

`NetStackConfig` may be a JSON string; it is percent-decoded before parsing.
No pointer constants or byte patching are involved in that path.

## Bounded self-test

A test-only entry point runs **one** fresh route and **one** CONNECT; it does
not instantiate the engine, load wasm, or launch a client:

```sh
cd v2-wasmtime
PLACE=189707 python3 ../tools/quiche-probe/fresh_route.py > /tmp/opencode/rust-wt-route.json
WC_WT_SELFTEST=1 WC_WT_ROUTE=/tmp/opencode/rust-wt-route.json \
  cargo test --bin alcohol wt_selftest -- --nocapture
```

If `WC_WT_ROUTE` is unset the test invokes `fresh_route.py` itself. The route
is generated immediately before the run because qdmux routes expire within
minutes. Gate: `WC_WT_SELFTEST` must be set, so normal `cargo test` is inert. There is
no in-binary early exit because `main.rs` is read-only for this change. If a
binary-mode flag is wanted later, the exact edit is at the top of `run()`:

```rust
fn run() -> Result<()> {
    if std::env::var_os("WC_WT_SELFTEST").is_some() { return wt::run_selftest(); }
    // ... existing body
}
```

and `wt::run_selftest()` can be the body of the test above, factored out of
`#[cfg(test)]`.

Evidence: see "Self-test evidence" below.

## Self-test evidence

Command (build once, then run with a route generated immediately beforehand):

```sh
cd v2-wasmtime
cargo test --bin alcohol --no-run
PLACE=189707 python3 ../tools/quiche-probe/fresh_route.py > /tmp/opencode/rust-wt-route.json
WC_WT_SELFTEST=1 WC_WT_ROUTE=/tmp/opencode/rust-wt-route.json \
  ./target/debug/deps/alcohol-<hash> wt_selftest --nocapture
```

Fresh route used (2026-11-26, generated seconds before the run; note the
browser `.34` address, not the native `.33` one):

```json
{
  "fqdn": "5e27dee8db32f63f32311ae6c379d94c-0a2507df-fa65.80740222.qdmux.roblox.com",
  "nativeHost": "128.116.2.33",
  "browserHost": "128.116.2.34",
  "netstackPort": 64101,
  "caps": "120ddbc750b31896",
  "certHashes": ["PbZ/67himMfGzHo4ikOXQSIUP43kNwG+tvisEqlTCEg="]
}
```

Verbatim output (one run, 0.17 s, no engine/wasm/client):

```
running 1 test
[selftest] route fqdn=5e27dee8db32f63f32311ae6c379d94c-0a2507df-fa65.80740222.qdmux.roblox.com peer=128.116.2.34:64101 caps=120ddbc750b31896 pins=1
[wt   ] pinning peer certificate to 1 SHA-256 hash(es)
[wt   ] quic connected peer=128.116.2.34:64101 sni=5e27dee8db32f63f32311ae6c379d94c-0a2507df-fa65.80740222.qdmux.roblox.com port=64101 maxdg=1150
[wt   ] x-rbx-capabilities=120ddbc750b31896
[wt   ] connect response 33 bytes
[wt   ] session established id=0
[wt   ] stream=0 :status 200
[wt   ] CONNECT stream=0 :status 200
[selftest] CONNECT status=Some(200) session=Some(0) maxdg=Some(1149)
test wt::tests::wt_selftest ... ok
test result: ok. 1 passed; 0 failed; 0 ignored; 0 measured; 1 filtered out
```

This proves, from this crate alone: QUIC handshake to the browser/UDMUX peer
with the qdmux FQDN as SNI, SHA-256 certificate-pin acceptance, draft-02
SETTINGS, extended CONNECT with the caps header, `:status 200`, session id 0,
and the datagram budget adjusted by the session-id varint (1150 → 1149).

`crypto_and_varint_vectors` (always-on test) also passes; it caught and now
guards a latent bug: `read_varint` previously consumed `1 << prefix`
continuation bytes instead of `(1 << prefix) - 1`, so **all** incoming WT
stream headers and all datagram prefixes were misparsed. That bug is fixed.

## What is proven

- A native Rust CONNECT reaches `:status 200` from a fresh gamejoin route
  without any browser, Deno, game client or wasm patching (evidence above).
- The same route inputs used by the quiche probe (browser `.34` peer, qdmux
  FQDN, caps, cert hash) are accepted by `ConnectOptions` and verified by
  `PinVerifier`.
- The H3/CONNECT wire format matches `draftwt.ts` and the quiche probe
  (headers above; `0x54`/`0x41` framing; session-id varint datagram prefix).
- QUIC keepalive/idle transport parameters are now configured, matching the
  proven draft-02 client (the quiche longevity probe used 5 s / 60 s and
  survived 240 s).

## Remaining gaps

1. **Guest completion path (JSPI/fibre)** — a different worktree is fixing
   the JSPI/fibre problem that stopped the engine before
   `rbx_wasm_rna_connect_js`. This crate cannot prove the RNA callback path
   end-to-end until that lands.
2. **`http.rs` must capture every join reply.** Today
   `store_join_script(&body)` only runs under `WC_RAKNET`
   (`v2-wasmtime/src/imports/http.rs:723`). For the RNA path, change that
   condition to store on every successful join (or at least whenever
   `WC_WT_SELFTEST`/the RNA backend is enabled):
   ```rust
   if status == 200 && is_join(&req.url) {
       store_join_script(&body);
   }
   ```
   Without it, `rna.rs` still works from the guest `certs` array and the caps
   in the URL, but loses the browser `.34` peer override and DNS may pick the
   wrong address.
3. **H3 SETTINGS subset.** shiguredo_http3 advertises only
   `0x2b603742` for draft-02; the quiche probe also sent `0xffd277`,
   `0xc671706a` and `0x2b603743`, of which the server echoed the latter two.
   The prior force-connect experiment reached `:status 200` with shiguredo's
   subset, so the extra ids appear optional, but this has not been re-run
   with a fresh route after the SETTINGS change.
4. **Bidi streams.** `draftwt.ts` also defines `0x41 <sid>` framing; `wt.rs`
   implements uni streams (the only kind the RNA bridge opens) and parses
   incoming `0x41` headers, but exposes no `open_bi`.
5. **WebTransport flow control capsules** (draft-14+) and QPACK dynamic table
   lifetime are delegated to shiguredo; the qdmux engine has not been driven
   far enough to exercise multi-megabyte DataModel streams here.
6. **Hardcoded guest ABI constants remain in `rna.rs`**: the ring-to-conn
   offset (`RING_DATA = 336`) and the table slot of the engine's ring consumer
   (`CONSUMER = 135389`). These are engine ABI facts, not route data; removing
   them requires discovering the consumer table slot at runtime and is out of
   scope for this change.
