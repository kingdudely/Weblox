//! WebTransport draft-02 over QUIC for the engine's `rbx_wasm_rna_*` imports.
//!
//! Shiguredo decodes H3 SETTINGS/QPACK and the CONNECT response; quinn owns the
//! socket.  Wire framing matches the proven Deno `page/draftwt.ts`: uni streams
//! start with 0x54 + session id, bidi with 0x41 + session id, and datagrams
//! carry just the session-id varint prefix.
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Result};
use quinn::{Connection as QConn, Endpoint as QEndpoint, RecvStream as QRecv, SendStream as QSend, VarInt};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::DigitallySignedStruct;
use shiguredo_http3::{ClientConnection, Event, Header, Settings as H3Settings};
use tokio::sync::{mpsc, Mutex as AsyncMutex};

static MAX_DG: AtomicU64 = AtomicU64::new(1200);

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

pub struct ClientConfig;
impl ClientConfig { pub fn builder() -> ConfigBuilder { ConfigBuilder } }
pub struct ConfigBuilder;
impl ConfigBuilder {
    pub fn with_bind_default(self) -> Self { self }
    pub fn with_no_cert_validation(self) -> Self { self }
    pub fn build(self) -> ClientConfig { ClientConfig }
}

pub struct Endpoint { ep: QEndpoint }
impl Endpoint {
    pub fn client(_cfg: ClientConfig) -> Result<Self> {
        let crypto = rustls::ClientConfig::builder()
            .dangerous().with_custom_certificate_verifier(Arc::new(NoVerify))
            .with_no_client_auth();
        let mut crypto = crypto;
        crypto.alpn_protocols = vec![b"h3".to_vec()];
        let mut cfg = quinn::ClientConfig::new(Arc::new(
            quinn::crypto::rustls::QuicClientConfig::try_from(crypto)?));
        let mut tp = quinn::TransportConfig::default();
        tp.datagram_receive_buffer_size(Some(256 * 1024));
        tp.datagram_send_buffer_size(256 * 1024);
        cfg.transport_config(Arc::new(tp));
        let mut ep = QEndpoint::client("0.0.0.0:0".parse()?)?;
        ep.set_default_client_config(cfg);
        Ok(Self { ep })
    }

    pub async fn connect(&self, url: &str) -> Result<Connection> {
        let u: url::Url = url.parse()?;
        let host = u.host_str().ok_or_else(|| anyhow!("no host in {url}"))?.to_string();
        let port = u.port_or_known_default().unwrap_or(443);
        let addr = tokio::net::lookup_host((host.as_str(), port)).await?
            .next().ok_or_else(|| anyhow!("no address for {host}"))?;
        let qc = self.ep.connect(addr, &host)?.await?;
        let max = qc.max_datagram_size().unwrap_or(1200) as u64;
        MAX_DG.store(max, Ordering::Relaxed);
        eprintln!("[wt   ] quic connected host={host} port={port} maxdg={max}");

        let wt = shiguredo_http3::webtransport::Settings::new().enable_webtransport_draft02(true);
        let mut h3 = ClientConnection::new(H3Settings::new().enable_webtransport_client(wt));
        let init = h3.init_h3_streams(2, 6, 10)?;
        let mut ctrl = qc.open_uni().await?;
        ctrl.write_all(&init.control_data).await?;
        let mut enc = qc.open_uni().await?;
        enc.write_all(&init.encoder_data).await?;
        let mut dec = qc.open_uni().await?;
        dec.write_all(&init.decoder_data).await?;

        let (cw, cr) = qc.open_bi().await?;

        let (new_uni_tx, new_uni_rx) = mpsc::unbounded_channel();
        let (dg_tx, _) = tokio::sync::broadcast::channel(1024);
        let dg_rx = dg_tx.subscribe();
        let shared = Arc::new(Shared {
            qc: qc.clone(),
            h3: Mutex::new(h3),
            session: Mutex::new(None),
            ready: tokio::sync::Notify::new(),
            new_uni_rx: AsyncMutex::new(new_uni_rx),
            dg_tx,
        });

        // Keep critical streams alive; forward any later H3 bytes on them.
        let mut cw = cw;
        let s = shared.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_millis(2)).await;
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
        let got = tokio::time::timeout(std::time::Duration::from_secs(5), async {
            loop {
                if s.h3.lock().unwrap().peer_settings().is_some() { break }
                tokio::time::sleep(std::time::Duration::from_millis(5)).await;
            }
        }).await;
        if got.is_err() { return Err(anyhow!("peer SETTINGS not received")); }
        let mut path = u.path().to_string();
        if let Some(q) = u.query() { path.push('?'); path.push_str(q); }
        let headers = vec![
            Header::new(b":method", b"CONNECT")?,
            Header::new(b":protocol", b"webtransport")?,
            Header::new(b":scheme", b"https")?,
            Header::new(b":authority", host.as_bytes())?,
            Header::new(b":path", path.as_bytes())?,
            Header::new(b"sec-webtransport-http3-draft02", b"1")?,
        ];
        shared.h3.lock().unwrap().set_webtransport_transport_verified(true, false)?;
        shared.h3.lock().unwrap().send_request(&headers, false)?;

        tokio::time::timeout(std::time::Duration::from_secs(10), shared.ready.notified()).await.ok();
        Ok(Connection { shared, dg_rx })
    }
}

struct Shared {
    qc: QConn,
    h3: Mutex<ClientConnection>,
    session: Mutex<Option<u64>>,
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
                    Event::Header { stream_id: 0, name, value } if name == b":status" => {
                        eprintln!("[wt   ] :status {}", String::from_utf8_lossy(&value));
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
            let mut first = true;
            let mut buf = vec![0u8; 64 * 1024];
            loop {
                match st.read(&mut buf).await {
                    Ok(Some(n)) => {
                        let mut p = 0;
                        if first {
                            if buf.is_empty() { continue; }
                            let t = buf[0]; p = 1;
                            if t == 0x54 { let _ = read_varint(&buf, &mut p); }
                            first = false;
                        }
                        if p < n { let _ = tx.send(Some(buf[p..n].to_vec())); }
                        let _ = &s2;
                    }
                    Ok(None) => { let _ = tx.send(None); break; }
                    Err(_) => break,
                }
            }
        });
    }
}

async fn read_dgrams(s: Arc<Shared>) {
    loop {
        let d = match s.qc.read_datagram().await { Ok(d) => d, Err(_) => break };
        let b: &[u8] = &d;
        let mut p = 0;
        if let Some(sid) = read_varint(b, &mut p) {
            if Some(sid) != *s.session.lock().unwrap() && p > 0 { continue; }
        }
        if p < b.len() { let _ = s.dg_tx.send(b[p..].to_vec()); }
    }
}

fn read_varint(buf: &[u8], p: &mut usize) -> Option<u64> {
    let b = *buf.get(*p)?; *p += 1;
    let (n, len) = match b >> 6 { 0 => (b as u64 & 63, 0), 1 => (b as u64 & 63, 1),
                                 2 => (b as u64 & 63, 2), _ => (b as u64 & 63, 3) };
    let mut v = n;
    for _ in 0..(1usize << len) { v = (v << 8) | *buf.get(*p)? as u64; *p += 1; }
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

pub struct SendStream { sid: u64, qc: QConn }
impl SendStream {
    pub async fn write_all(&mut self, data: &[u8]) -> std::io::Result<()> {
        let mut s: QSend = self.qc.open_uni().await.map_err(io_err)?;
        s.write_all(&[0x54]).await.map_err(io_err)?;
        s.write_all(&varint(self.sid)).await.map_err(io_err)?;
        s.write_all(data).await.map_err(io_err)?;
        Ok(())
    }
    pub async fn finish(&mut self) -> Result<()> { Ok(()) }
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
    pub async fn accept_uni(&self) -> std::io::Result<RecvStream> {
        let rx = self.shared.new_uni_rx.lock().await.recv().await
            .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::UnexpectedEof, "closed"))?;
        Ok(RecvStream { rx, buf: Vec::new() })
    }
    pub async fn open_uni(&self) -> Result<SendStream> {
        let sid = self.shared.session.lock().unwrap().unwrap_or(0);
        Ok(SendStream { sid, qc: self.shared.qc.clone() })
    }
    pub fn send_datagram(&self, data: &[u8]) -> Result<(), ()> {
        let sid = self.shared.session.lock().unwrap().unwrap_or(0);
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
