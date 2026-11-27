//! WebTransport draft-02 over QUIC for the engine's `rbx_wasm_rna_*` imports.
//!
//! Shiguredo decodes H3 SETTINGS/QPACK and the CONNECT response; quinn owns the
//! socket.  Wire framing matches the proven Deno `page/draftwt.ts`: uni streams
//! start with 0x54 + session id, bidi with 0x41 + session id, and datagrams
//! carry just the session-id varint prefix.
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{anyhow, Result};
use quinn::{Connection as QConn, Endpoint as QEndpoint, IdleTimeout, RecvStream as QRecv,
            SendStream as QSend, VarInt};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{CertificateError, DigitallySignedStruct};
use shiguredo_http3::{ClientConnection, Event, Header, Settings as H3Settings};
use tokio::sync::{mpsc, Mutex as AsyncMutex};

static MAX_DG: AtomicU64 = AtomicU64::new(1200);

/// Minimal, dependency-free SHA-256.  The peer certificate is pinned against
/// `certHashes` from the join script, exactly like Chrome's
/// `serverCertificateHashes`; rustls keeps CA validation off for this qdmux
/// endpoint and this pin is the only authentication we get.
fn sha256(input: &[u8]) -> [u8; 32] {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ];
    let mut h: [u32; 8] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    let mut msg = input.to_vec();
    let bits = (msg.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 { msg.push(0); }
    msg.extend_from_slice(&bits.to_be_bytes());
    for c in msg.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([c[4*i], c[4*i+1], c[4*i+2], c[4*i+3]]);
        }
        for i in 16..64 {
            let s0 = w[i-15].rotate_right(7) ^ w[i-15].rotate_right(18) ^ (w[i-15] >> 3);
            let s1 = w[i-2].rotate_right(17) ^ w[i-2].rotate_right(19) ^ (w[i-2] >> 10);
            w[i] = w[i-16].wrapping_add(s0).wrapping_add(w[i-7]).wrapping_add(s1);
        }
        let (mut a, mut b, mut c0, mut d, mut e, mut f, mut g, mut hh) =
            (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = hh.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c0) ^ (b & c0);
            let t2 = s0.wrapping_add(maj);
            hh = g; g = f; f = e; e = d.wrapping_add(t1);
            d = c0; c0 = b; b = a; a = t1.wrapping_add(t2);
        }
        h[0] = h[0].wrapping_add(a); h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c0); h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e); h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g); h[7] = h[7].wrapping_add(hh);
    }
    let mut out = [0u8; 32];
    for i in 0..8 { out[4*i..4*i+4].copy_from_slice(&h[i].to_be_bytes()); }
    out
}

/// Decode the base64 SHA-256 values in `NetStackConfig.certHashes`.
fn b64_decode(s: &str) -> Option<Vec<u8>> {
    let val = |c: u8| -> Option<u8> {
        match c {
            b'A'..=b'Z' => Some(c - b'A'),
            b'a'..=b'z' => Some(c - b'a' + 26),
            b'0'..=b'9' => Some(c - b'0' + 52),
            b'+' => Some(62), b'/' => Some(63),
            _ => None,
        }
    };
    let mut out = Vec::new();
    let mut acc = 0u32;
    let mut bits = 0u32;
    for &c in s.as_bytes() {
        if c == b'=' { break; }
        acc = (acc << 6) | val(c)? as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1u32 << bits) - 1;
        }
    }
    Some(out)
}

#[derive(Debug)]
struct NoVerify;
impl ServerCertVerifier for NoVerify {
    fn verify_server_cert(&self, _: &CertificateDer<'_>, _: &[CertificateDer<'_>],
        _: &ServerName<'_>, _: &[u8], _: UnixTime) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }
    fn verify_tls12_signature(&self, _: &[u8], _: &CertificateDer<'_>, _: &DigitallySignedStruct)
        -> Result<HandshakeSignatureValid, rustls::Error> { Ok(HandshakeSignatureValid::assertion()) }
    fn verify_tls13_signature(&self, _: &[u8], _: &CertificateDer<'_>, _: &DigitallySignedStruct)
        -> Result<HandshakeSignatureValid, rustls::Error> { Ok(HandshakeSignatureValid::assertion()) }
    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        vec![rustls::SignatureScheme::RSA_PSS_SHA256, rustls::SignatureScheme::ECDSA_NISTP256_SHA256,
             rustls::SignatureScheme::ED25519]
    }
}

#[derive(Debug)]
struct PinVerifier { hashes: Vec<[u8; 32]> }
impl ServerCertVerifier for PinVerifier {
    fn verify_server_cert(&self, cert: &CertificateDer<'_>, _: &[CertificateDer<'_>],
        _: &ServerName<'_>, _: &[u8], _: UnixTime) -> Result<ServerCertVerified, rustls::Error> {
        let got = sha256(cert.as_ref());
        if self.hashes.contains(&got) {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::InvalidCertificate(
                CertificateError::ApplicationVerificationFailure))
        }
    }
    fn verify_tls12_signature(&self, _: &[u8], _: &CertificateDer<'_>, _: &DigitallySignedStruct)
        -> Result<HandshakeSignatureValid, rustls::Error> { Ok(HandshakeSignatureValid::assertion()) }
    fn verify_tls13_signature(&self, _: &[u8], _: &CertificateDer<'_>, _: &DigitallySignedStruct)
        -> Result<HandshakeSignatureValid, rustls::Error> { Ok(HandshakeSignatureValid::assertion()) }
    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        vec![rustls::SignatureScheme::RSA_PSS_SHA256, rustls::SignatureScheme::ECDSA_NISTP256_SHA256,
             rustls::SignatureScheme::ED25519]
    }
}

/// Everything route-specific the engine learns from `joinScript`.  No value is
/// baked into this crate: the URL, caps and cert hashes all come from the join
/// reply, and the peer address is the browser/UDMUX `.34` endpoint, never the
/// DNS result of the qdmux FQDN (which is only TLS SNI and `:authority`).
#[derive(Default, Clone, Debug)]
pub struct ConnectOptions {
    pub peer: Option<SocketAddr>,
    pub sni: Option<String>,
    pub authority: Option<String>,
    pub caps: Option<String>,
    /// Kept with the route for callers that want to inspect it; the actual
    /// TLS verification is configured once on `ClientConfig`.
    pub cert_hashes: Vec<String>,
}

pub struct ClientConfig {
    cert_hashes: Vec<String>,
    keepalive: Duration,
    idle_timeout: Duration,
}
impl ClientConfig { pub fn builder() -> ConfigBuilder { ConfigBuilder::default() } }

#[derive(Default)]
pub struct ConfigBuilder {
    cert_hashes: Vec<String>,
    keepalive: Option<Duration>,
    idle_timeout: Option<Duration>,
}
impl ConfigBuilder {
    pub fn with_bind_default(self) -> Self { self }
    /// Compatibility name: hashes are now pinned instead of skipping all checks.
    pub fn with_no_cert_validation(self) -> Self { self }
    /// base64 SHA-256 DER hashes from `NetStackConfig.certHashes`.
    pub fn with_cert_hashes(mut self, hashes: Vec<String>) -> Self {
        self.cert_hashes.extend(hashes);
        self
    }
    /// `keepAliveInterval` from the proven draft-02 client (4 s).
    pub fn with_keepalive(mut self, d: Duration) -> Self { self.keepalive = Some(d); self }
    pub fn with_idle_timeout(mut self, d: Duration) -> Self { self.idle_timeout = Some(d); self }
    pub fn build(self) -> ClientConfig {
        ClientConfig {
            cert_hashes: self.cert_hashes,
            keepalive: self.keepalive.unwrap_or(Duration::from_secs(4)),
            idle_timeout: self.idle_timeout.unwrap_or(Duration::from_secs(10)),
        }
    }
}

pub struct Endpoint { ep: QEndpoint }
impl Endpoint {
    pub fn client(cfg: ClientConfig) -> Result<Self> {
        // With no pins this keeps the old permissive behaviour; with pins the
        // peer certificate DER must hash to one of them (Chrome parity).
        let pins: Vec<[u8; 32]> = cfg.cert_hashes.iter()
            .filter_map(|s| b64_decode(s))
            .filter_map(|d| <[u8; 32]>::try_from(d.as_slice()).ok())
            .collect();
        let verifier: Arc<dyn ServerCertVerifier> = if pins.is_empty() {
            Arc::new(NoVerify)
        } else {
            eprintln!("[wt   ] pinning peer certificate to {} SHA-256 hash(es)", pins.len());
            Arc::new(PinVerifier { hashes: pins })
        };
        let mut crypto = rustls::ClientConfig::builder()
            .dangerous().with_custom_certificate_verifier(verifier)
            .with_no_client_auth();
        crypto.alpn_protocols = vec![b"h3".to_vec()];
        let mut qcfg = quinn::ClientConfig::new(Arc::new(
            quinn::crypto::rustls::QuicClientConfig::try_from(crypto)?));
        let mut tp = quinn::TransportConfig::default();
        tp.datagram_receive_buffer_size(Some(256 * 1024));
        tp.datagram_send_buffer_size(256 * 1024);
        // Proven draft-02 client: keepAliveInterval 4 s, maxIdleTimeout 10 s.
        // Keepalive keeps both idle timers alive while the engine is booting.
        tp.keep_alive_interval(Some(cfg.keepalive));
        if let Ok(idle) = IdleTimeout::try_from(cfg.idle_timeout) {
            tp.max_idle_timeout(Some(idle));
        }
        qcfg.transport_config(Arc::new(tp));
        let mut ep = QEndpoint::client("0.0.0.0:0".parse()?)?;
        ep.set_default_client_config(qcfg);
        Ok(Self { ep })
    }

    /// URL-only path, kept for callers that do not have a join script.
    pub async fn connect(&self, url: &str) -> Result<Connection> {
        self.connect_with(url, ConnectOptions::default()).await
    }

    /// Connect with the route parameters extracted from `JOIN_SCRIPT`.
    /// `peer` overrides DNS (the qdmux FQDN is for SNI/`:authority` only),
    /// `caps` becomes `x-rbx-capabilities`, and `cert_hashes` pin TLS.
    pub async fn connect_with(&self, url: &str, opts: ConnectOptions) -> Result<Connection> {
        let u: url::Url = url.parse()?;
        let host = opts.sni.clone().unwrap_or_else(|| {
            u.host_str().unwrap_or_default().to_string()
        });
        let port = u.port_or_known_default().unwrap_or(443);
        let addr = match opts.peer {
            Some(a) => a,
            None => tokio::net::lookup_host((host.as_str(), port)).await?
                .next().ok_or_else(|| anyhow!("no address for {host}"))?,
        };
        let qc = self.ep.connect(addr, &host)?.await?;
        let max = qc.max_datagram_size().unwrap_or(1200) as u64;
        MAX_DG.store(max, Ordering::Relaxed);
        eprintln!("[wt   ] quic connected peer={addr} sni={host} port={port} maxdg={max}");

        let wt = shiguredo_http3::webtransport::Settings::new().enable_webtransport_draft02(true);
        let settings = H3Settings::new()
            .enable_webtransport_client(wt)
            .enable_connect_protocol(true)
            .max_field_section_size(shiguredo_http3::VarInt::from_static(65536))
            .qpack_max_table_capacity(shiguredo_http3::VarInt::from_static(65536))
            .qpack_blocked_streams(shiguredo_http3::VarInt::from_static(100));
        let mut h3 = ClientConnection::new(settings);
        // shiguredo's client convention: local uni ids 2/6/10 (RFC 9114 IDs
        // 0x2, 0x2+4, 0x2+8).  The peer's uni streams are 3/7/11.
        let init = h3.init_h3_streams(2, 6, 10)?;
        let mut ctrl = qc.open_uni().await?;
        ctrl.write_all(&init.control_data).await?;
        let mut enc = qc.open_uni().await?;
        enc.write_all(&init.encoder_data).await?;
        let mut dec = qc.open_uni().await?;
        dec.write_all(&init.decoder_data).await?;

        let (cw, cr) = qc.open_bi().await?;
        let connect_stream = cw.id().index();

        let (new_uni_tx, new_uni_rx) = mpsc::unbounded_channel();
        let (dg_tx, _) = tokio::sync::broadcast::channel(1024);
        let dg_rx = dg_tx.subscribe();
        let shared = Arc::new(Shared {
            qc: qc.clone(),
            h3: Mutex::new(h3),
            session: Mutex::new(None),
            status: Mutex::new(None),
            ready: tokio::sync::Notify::new(),
            new_uni_rx: AsyncMutex::new(new_uni_rx),
            dg_tx,
        });

        // Keep critical streams alive; forward any later H3 bytes on them.
        let mut cw = cw;
        let s = shared.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(Duration::from_millis(2)).await;
                let data: Vec<(u64, Vec<u8>, bool)> = {
                    let mut h = s.h3.lock().unwrap();
                    let mut v = Vec::new();
                    for id in [0u64, 2, 6, 10] {
                        if let Some((d, fin)) = h.take_stream_data(id) {
                            if !d.is_empty() || fin { v.push((id, d, fin)); }
                        }
                    }
                    v
                };
                for (id, d, fin) in data {
                    match id {
                        0 => { let _ = cw.write_all(&d).await; if fin { let _ = cw.finish(); } }
                        2 => { let _ = ctrl.write_all(&d).await; }
                        6 => { let _ = enc.write_all(&d).await; }
                        _ => { let _ = dec.write_all(&d).await; }
                    }
                }
            }
        });
        // Read the CONNECT response and feed it to shiguredo.
        let s = shared.clone();
        tokio::spawn(async move { read_connect(s, cr).await; });
        // Poll shiguredo events (session established, datagrams).
        let s = shared.clone();
        tokio::spawn(async move { poll_events(s).await; });
        // Accept peer uni streams and route them.
        let s = shared.clone();
        tokio::spawn(async move { accept_uni(s, new_uni_tx).await; });
        // QUIC datagrams -> session-id-prefixed payloads.
        let s = shared.clone();
        tokio::spawn(async move { read_dgrams(s).await; });

        // shiguredo refuses a WebTransport CONNECT until the peer's H3
        // SETTINGS have been fed in by the stream pumps above.
        let s = shared.clone();
        let got = tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                if s.h3.lock().unwrap().peer_settings().is_some() { break }
                tokio::time::sleep(Duration::from_millis(5)).await;
            }
        }).await;
        if got.is_err() { return Err(anyhow!("peer SETTINGS not received")); }
        let mut path = u.path().to_string();
        if let Some(q) = u.query() { path.push('?'); path.push_str(q); }
        let authority = opts.authority.unwrap_or_else(|| {
            format!("{}:{}", u.host_str().unwrap_or_default(), port)
        });
        // The engine puts its capability set in the /wt URL as a percent-
        // encoded query parameter; Chrome sends the same value as a header.
        let caps = opts.caps.or_else(|| {
            u.query_pairs().find(|(k, _)| k.eq_ignore_ascii_case("x-rbx-capabilities"))
                .map(|(_, v)| v.into_owned())
        });
        let mut headers = vec![
            Header::new(b":method", b"CONNECT")?,
            Header::new(b":protocol", b"webtransport")?,
            Header::new(b":scheme", b"https")?,
            Header::new(b":authority", authority.as_bytes())?,
            Header::new(b":path", path.as_bytes())?,
            Header::new(b"sec-webtransport-http3-draft02", b"1")?,
        ];
        if let Some(caps) = caps.filter(|c| !c.is_empty()) {
            eprintln!("[wt   ] x-rbx-capabilities={caps}");
            headers.push(Header::new(b"x-rbx-capabilities", caps.as_bytes())?);
        }
        shared.h3.lock().unwrap().set_webtransport_transport_verified(true, false)?;
        shared.h3.lock().unwrap().send_request(&headers, false)?;

        let notified = tokio::time::timeout(Duration::from_secs(10), shared.ready.notified()).await;
        if notified.is_err() {
            return Err(anyhow!("CONNECT timed out (status {:?})",
                *shared.status.lock().unwrap()));
        }
        // shiguredo emits SessionEstablished before the :status header event;
        // give it a beat so callers can report a real CONNECT status.
        for _ in 0..50 {
            if shared.status.lock().unwrap().is_some() { break }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        let status = *shared.status.lock().unwrap();
        match status {
            Some(s) if !(200..300).contains(&s) =>
                return Err(anyhow!("CONNECT :status {s}")),
            Some(s) => eprintln!("[wt   ] CONNECT stream={connect_stream} :status {s}"),
            None => eprintln!("[wt   ] CONNECT stream={connect_stream} session established (no :status event)"),
        }
        // The session id is the CONNECT stream's QUIC id (draft-02 §4).
        if let Some(sid) = *shared.session.lock().unwrap() {
            let prefix = varint(sid).len();
            MAX_DG.store(max.saturating_sub(prefix as u64), Ordering::Relaxed);
        }
        Ok(Connection { shared, dg_rx })
    }
}

struct Shared {
    qc: QConn,
    h3: Mutex<ClientConnection>,
    session: Mutex<Option<u64>>,
    status: Mutex<Option<u16>>,
    ready: tokio::sync::Notify,
    new_uni_rx: AsyncMutex<mpsc::UnboundedReceiver<mpsc::UnboundedReceiver<Option<Vec<u8>>>>>,
    dg_tx: tokio::sync::broadcast::Sender<Vec<u8>>,
}

async fn read_connect(s: Arc<Shared>, mut cr: QRecv) {
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        match cr.read(&mut buf).await {
            Ok(Some(n)) => {
                eprintln!("[wt   ] connect response {n} bytes");
                let _ = s.h3.lock().unwrap().feed_stream(0, &buf[..n], false);
            }
            Ok(None) => { let _ = s.h3.lock().unwrap().feed_stream(0, &[], true); break; }
            Err(e) => { eprintln!("[wt   ] connect read: {e}"); break; }
        }
    }
}

async fn poll_events(s: Arc<Shared>) {
    loop {
        let evs = { let mut h = s.h3.lock().unwrap(); h.drain_events() };
        match evs {
            Ok(events) => for e in events {
                match e {
                    Event::WebTransport(shiguredo_http3::WebTransportEvent::SessionEstablished { session_id, .. }) => {
                        eprintln!("[wt   ] session established id={session_id}");
                        *s.session.lock().unwrap() = Some(session_id);
                        s.ready.notify_waiters();
                    }
                    Event::Header { stream_id, name, value } if name == b":status" => {
                        eprintln!("[wt   ] stream={stream_id} :status {}", String::from_utf8_lossy(&value));
                        if let Ok(code) = String::from_utf8_lossy(&value).parse::<u16>() {
                            *s.status.lock().unwrap() = Some(code);
                            s.ready.notify_waiters();
                        }
                    }
                    Event::WebTransport(shiguredo_http3::WebTransportEvent::Datagram { payload, .. }) => {
                        let _ = s.dg_tx.send(payload);
                    }
                    Event::StreamReset { stream_id, error_code } => {
                        eprintln!("[wt   ] reset stream={stream_id} code={error_code}");
                    }
                    _ => {}
                }
            },
            Err(e) => { eprintln!("[wt   ] h3 events: {e:?}"); break; }
        }
        tokio::time::sleep(std::time::Duration::from_millis(2)).await;
    }
}

async fn accept_uni(s: Arc<Shared>, new: mpsc::UnboundedSender<mpsc::UnboundedReceiver<Option<Vec<u8>>>>) {
    loop {
        let mut st = match s.qc.accept_uni().await { Ok(x) => x, Err(_) => break };
        let s2 = s.clone();
        // index() is per-direction; server-initiated uni stream with index n
        // is H3/QUIC stream id 3 + 4*n (control = 3).
        let id = 3 + 4 * st.id().index();
        let (tx, rx) = mpsc::unbounded_channel();
        if id == 3 || id == 7 || id == 11 {
            tokio::spawn(async move {
                let mut buf = vec![0u8; 64 * 1024];
                loop {
                    match st.read(&mut buf).await {
                        Ok(Some(n)) => { let _ = s2.h3.lock().unwrap().feed_stream(id, &buf[..n], false); }
                        Ok(None) => { let _ = s2.h3.lock().unwrap().feed_stream(id, &[], true); break; }
                        Err(_) => break,
                    }
                }
            });
            continue;
        }
        let _ = new.send(rx);
        tokio::spawn(async move {
            // draft-02 framing: <type varint 0x54/0x41><session-id varint><payload>.
            // The two varints can straddle QUIC stream reads, so buffer them.
            let mut header = Vec::new();
            let mut stripping = true;
            let mut buf = vec![0u8; 64 * 1024];
            loop {
                let n = match st.read(&mut buf).await {
                    Ok(Some(n)) => n,
                    Ok(None) => { let _ = tx.send(None); break; }
                    Err(e) => { eprintln!("[wt   ] stream read: {e}"); break; }
                };
                if n == 0 { continue }
                if stripping {
                    header.extend_from_slice(&buf[..n]);
                    let mut p = 0;
                    let Some(t) = read_varint(&header, &mut p) else { continue };
                    if t != 0x54 && t != 0x41 {
                        eprintln!("[wt   ] uni stream type {t:#x} (not WT)");
                        break;
                    }
                    let Some(sid) = read_varint(&header, &mut p) else { continue };
                    let mine = *s2.session.lock().unwrap();
                    if Some(sid) != mine {
                        eprintln!("[wt   ] stream for session {sid}, ours {mine:?}");
                        break;
                    }
                    stripping = false;
                    if p < header.len() { let _ = tx.send(Some(header[p..].to_vec())); }
                    header.clear();
                } else {
                    let _ = tx.send(Some(buf[..n].to_vec()));
                }
            }
        });
    }
}

async fn read_dgrams(s: Arc<Shared>) {
    loop {
        let d = match s.qc.read_datagram().await { Ok(d) => d, Err(_) => break };
        let Some(sid) = *s.session.lock().unwrap() else { continue };
        // draft-02: the first varint is the session id; drop foreign datagrams.
        let prefix = varint(sid);
        if !d.starts_with(&prefix) { continue }
        if d.len() > prefix.len() { let _ = s.dg_tx.send(d[prefix.len()..].to_vec()); }
    }
}

fn read_varint(buf: &[u8], p: &mut usize) -> Option<u64> {
    let b = *buf.get(*p)?; *p += 1;
    // QUIC varint: the 2-bit prefix encodes the TOTAL length as 1 << prefix,
    // so the number of continuation bytes is (1 << prefix) - 1.
    let prefix = b >> 6;
    let extra = (1usize << prefix) - 1;
    let mut v = (b & 0x3f) as u64;
    for _ in 0..extra { v = (v << 8) | *buf.get(*p)? as u64; *p += 1; }
    Some(v)
}

fn varint(v: u64) -> Vec<u8> {
    if v < 64 { return vec![v as u8]; }
    if v < 16384 { return vec![0x40 | (v >> 8) as u8, v as u8]; }
    if v < (1 << 30) { return vec![0x80 | (v >> 24) as u8, (v >> 16) as u8, (v >> 8) as u8, v as u8]; }
    vec![0xc0 | (v >> 56) as u8, (v >> 48) as u8, (v >> 40) as u8, (v >> 32) as u8,
         (v >> 24) as u8, (v >> 16) as u8, (v >> 8) as u8, v as u8]
}

pub struct RecvStream { rx: mpsc::UnboundedReceiver<Option<Vec<u8>>>, buf: Vec<u8> }
impl RecvStream {
    pub async fn read(&mut self, out: &mut [u8]) -> std::io::Result<Option<usize>> {
        loop {
            if !self.buf.is_empty() {
                let n = self.buf.len().min(out.len());
                out[..n].copy_from_slice(&self.buf[..n]);
                self.buf.drain(..n);
                return Ok(Some(n));
            }
            match self.rx.recv().await {
                Some(Some(b)) => self.buf.extend_from_slice(&b),
                Some(None) | None => return Ok(None),
            }
        }
    }
    pub fn stop(&self, _code: VarInt) {}
}

pub struct SendStream { sid: u64, qc: QConn, send: Option<QSend> }
impl SendStream {
    pub async fn write_all(&mut self, data: &[u8]) -> std::io::Result<()> {
        // One QUIC uni stream per logical stream.  The engine writes many
        // chunks; re-opening per write would duplicate the 0x54 <sid> header.
        if self.send.is_none() {
            let mut s: QSend = self.qc.open_uni().await.map_err(io_err)?;
            s.write_all(&[0x54]).await.map_err(io_err)?;      // WT_UNI
            s.write_all(&varint(self.sid)).await.map_err(io_err)?;
            self.send = Some(s);
        }
        self.send.as_mut().unwrap().write_all(data).await.map_err(io_err)
    }
    pub async fn finish(&mut self) -> Result<()> {
        if let Some(s) = self.send.as_mut() { s.finish().map_err(io_err)?; }
        Ok(())
    }
}

fn io_err<E: std::fmt::Display>(e: E) -> std::io::Error { std::io::Error::other(e.to_string()) }

pub struct Connection { shared: Arc<Shared>, dg_rx: tokio::sync::broadcast::Receiver<Vec<u8>> }
impl Clone for Connection {
    fn clone(&self) -> Self {
        Self { shared: self.shared.clone(), dg_rx: self.shared.dg_tx.subscribe() }
    }
}
impl Connection {
    pub fn max_datagram_size(&self) -> Option<usize> { Some(MAX_DG.load(Ordering::Relaxed) as usize) }
    /// The HTTP/3 CONNECT `:status`, for diagnostics and the self-test.
    pub fn connect_status(&self) -> Option<u16> { *self.shared.status.lock().unwrap() }
    /// The draft-02 WebTransport session id (the CONNECT stream's QUIC id).
    pub fn session_id(&self) -> Option<u64> { *self.shared.session.lock().unwrap() }
    pub async fn accept_uni(&self) -> std::io::Result<RecvStream> {
        let rx = self.shared.new_uni_rx.lock().await.recv().await
            .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::UnexpectedEof, "closed"))?;
        Ok(RecvStream { rx, buf: Vec::new() })
    }
    pub async fn open_uni(&self) -> Result<SendStream> {
        let sid = self.shared.session.lock().unwrap().ok_or_else(|| anyhow!("no WT session"))?;
        Ok(SendStream { sid, qc: self.shared.qc.clone(), send: None })
    }
    pub fn send_datagram(&self, data: &[u8]) -> Result<(), ()> {
        let sid = self.shared.session.lock().unwrap().ok_or(())?;
        let mut d = varint(sid);
        d.extend_from_slice(data);
        self.shared.qc.send_datagram(d.into()).map_err(|_| ())
    }
    pub async fn receive_datagram(&mut self) -> Result<Datagram, ()> {
        match self.dg_rx.recv().await {
            Ok(d) => Ok(Datagram(d)),
            Err(tokio::sync::broadcast::error::RecvError::Lagged(n)) => {
                eprintln!("[wt   ] datagram lag {n}");
                Err(())
            }
            Err(_) => Err(()),
        }
    }
    pub async fn closed(&self) { self.shared.qc.closed().await; }
    pub fn close(&self, _code: VarInt, _msg: &[u8]) { self.shared.qc.close(0u32.into(), b""); }
}

pub struct Datagram(Vec<u8>);
impl Datagram { pub fn payload(&self) -> &[u8] { &self.0 } }

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crypto_and_varint_vectors() {
        // sha256("abc")
        let hex = sha256(b"abc").iter().map(|b| format!("{b:02x}")).collect::<String>();
        assert_eq!(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert_eq!(b64_decode("1yhOHKRKIH/y0x4IK/z+wTHHAHsnDFTCfE5v96v5obc=").unwrap().len(), 32);
        for v in [0u64, 1, 63, 64, 16383, 16384, 1 << 30, u64::MAX >> 2] {
            let mut p = 0;
            assert_eq!(read_varint(&varint(v), &mut p), Some(v));
            assert_eq!(p, varint(v).len());
        }
    }

    /// `WC_WT_SELFTEST=1 cargo test --bin alcohol wt_selftest -- --nocapture`
    ///
    /// Bounded: one fresh gamejoin route, one CONNECT, one status report.  It
    /// does not instantiate the engine, load wasm, or start any game client.
    #[test]
    fn wt_selftest() {
        if std::env::var_os("WC_WT_SELFTEST").is_none() { return }
        let route = match std::env::var("WC_WT_ROUTE") {
            Ok(path) => std::fs::read_to_string(path).expect("read WC_WT_ROUTE"),
            Err(_) => {
                let script = concat!(env!("CARGO_MANIFEST_DIR"), "/../tools/quiche-probe/fresh_route.py");
                let out = std::process::Command::new("python3").arg(script)
                    .env("PLACE", std::env::var("WC_PLACE").unwrap_or_else(|_| "189707".into()))
                    .output().expect("run fresh_route.py");
                assert!(out.status.success(), "fresh_route.py: {}", String::from_utf8_lossy(&out.stderr));
                String::from_utf8(out.stdout).expect("route utf8")
            }
        };
        let r: serde_json::Value = serde_json::from_str(&route).expect("route json");
        let fqdn = r["fqdn"].as_str().expect("fqdn").to_string();
        let port = r["netstackPort"].as_u64().expect("port") as u16;
        let browser = r["browserHost"].as_str().expect("browserHost");
        let peer: SocketAddr = format!("{browser}:{port}").parse().expect("peer addr");
        let caps = r["caps"].as_str().unwrap_or_default().to_string();
        let hashes: Vec<String> = match r["certHashes"].as_array() {
            Some(a) => a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect(),
            None => Vec::new(),
        };
        let url = format!("https://{fqdn}:{port}/wt");
        eprintln!("[selftest] route fqdn={fqdn} peer={peer} caps={caps} pins={}", hashes.len());

        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all().build().expect("tokio runtime");
        rt.block_on(async move {
            let cfg = ClientConfig::builder().with_bind_default()
                .with_cert_hashes(hashes).build();
            let ep = Endpoint::client(cfg).expect("endpoint");
            let opts = ConnectOptions {
                peer: Some(peer),
                sni: Some(fqdn.clone()),
                authority: Some(format!("{fqdn}:{port}")),
                caps: if caps.is_empty() { None } else { Some(caps) },
                cert_hashes: Vec::new(),
            };
            let result = tokio::time::timeout(Duration::from_secs(20), ep.connect_with(&url, opts)).await;
            match result {
                Ok(Ok(c)) => {
                    eprintln!("[selftest] CONNECT status={:?} session={:?} maxdg={:?}",
                        c.connect_status(), c.session_id(), c.max_datagram_size());
                    assert_eq!(c.connect_status(), Some(200));
                    assert!(c.session_id().is_some());
                    c.close(0u32.into(), b"selftest done");
                }
                Ok(Err(e)) => panic!("[selftest] connect failed: {e:#}"),
                Err(_) => panic!("[selftest] connect timed out after 20s"),
            }
        });
    }
}
