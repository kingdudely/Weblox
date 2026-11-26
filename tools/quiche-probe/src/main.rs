//! Minimal feasibility probe: can native libquiche complete the Roblox/qdmux
//! WebTransport (HTTP/3 extended CONNECT) handshake?
//!
//! This is a diagnostic harness, not a product transport. It uses the Rust
//! `quiche` crate (Cloudflare's libquiche + BoringSSL) and its built-in H3
//! implementation. It does NOT implement WebTransport streams/datagrams: the
//! probe stops as soon as the CONNECT response headers are received.

use std::io::{Read, Write};
use std::net::{IpAddr, SocketAddr, ToSocketAddrs as _, UdpSocket};
use std::time::{Duration, Instant};

use base64::Engine as _;
use quiche::h3::NameValue as _;
use sha2::Digest as _;

const MAX_DATAGRAM_SIZE: usize = 1472;

#[derive(Debug)]
struct Args {
    /// IP (or hostname) to send UDP packets to: the browser/UDMUX `.34` host.
    host: String,
    port: u16,
    /// TLS SNI and :authority host: the qdmux game FQDN.
    fqdn: String,
    caps: String,
    /// base64 SHA-256 of the peer certificate DER, from NetStackConfig.
    cert_hash: Option<String>,
    timeout_ms: u64,
    verbose: bool,
}

fn parse_args() -> Args {
    let mut host = String::new();
    let mut port = 55457u16;
    let mut fqdn = String::new();
    let mut caps = String::new();
    let mut cert_hash = None;
    let mut timeout_ms = 15_000u64;
    let mut verbose = false;

    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        match a.as_str() {
            "--host" => host = it.next().expect("--host value"),
            "--port" => port = it.next().expect("--port value").parse().unwrap(),
            "--fqdn" => fqdn = it.next().expect("--fqdn value"),
            "--caps" => caps = it.next().expect("--caps value"),
            "--cert-hash" => cert_hash = Some(it.next().expect("--cert-hash value")),
            "--timeout-ms" => timeout_ms = it.next().expect("--timeout-ms value").parse().unwrap(),
            "--verbose" | "-v" => verbose = true,
            "--help" | "-h" => {
                println!(
                    "usage: quiche-probe --host <ip> --port <port> --fqdn <qdmux-fqdn> \
                     [--caps <hex>] [--cert-hash <base64>] [--timeout-ms N] [-v]"
                );
                std::process::exit(0);
            }
            other => panic!("unknown argument {other:?} (try --help)"),
        }
    }

    if host.is_empty() || fqdn.is_empty() {
        panic!("--host and --fqdn are required (try --help)");
    }

    Args { host, port, fqdn, caps, cert_hash, timeout_ms, verbose }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// quiche's Config has no certificate-verification callback, so BoringSSL's
/// CA validation must be disabled. We compensate by comparing the SHA-256 of
/// the DER certificate BoringSSL actually received with the route's pinned
/// WebTransport certificate hash. This is the same hash Chrome checks.
fn check_pinned_cert(conn: &quiche::Connection, pinned_b64: Option<&str>) -> Result<(), String> {
    let Some(pinned) = pinned_b64 else {
        return Ok(());
    };
    let der = conn
        .peer_cert()
        .ok_or_else(|| "no peer certificate in QUIC handshake".to_string())?;
    let digest = sha2::Sha256::digest(der);
    let got = base64::engine::general_purpose::STANDARD.encode(digest);
    if got != pinned {
        return Err(format!(
            "certificate hash mismatch: pinned={pinned} got={got} (DER {} bytes, sha256 {})",
            der.len(),
            hex(&digest)
        ));
    }
    println!("[cert] SHA-256 DER hash matches pinned route hash ({got})");
    Ok(())
}

fn main() {
    let args = parse_args();

    // Resolve the browser/UDMUX host explicitly. The qdmux FQDN may not resolve
    // to the required `.34` multiplexer, so we never resolve the FQDN here; it
    // is used only for SNI and :authority.
    let peer_ip: IpAddr = match args.host.parse() {
        Ok(ip) => ip,
        Err(_) => (args.host.as_str(), args.port)
            .to_socket_addrs()
            .expect("failed to resolve --host")
            .next()
            .expect("no address for --host")
            .ip(),
    };
    let peer_addr = SocketAddr::new(peer_ip, args.port);
    let bind_addr: SocketAddr = match peer_addr {
        SocketAddr::V4(_) => "0.0.0.0:0".parse().unwrap(),
        SocketAddr::V6(_) => "[::]:0".parse().unwrap(),
    };

    let socket = UdpSocket::bind(bind_addr).expect("bind UDP socket");
    let local_addr = socket.local_addr().unwrap();

    println!(
        "[quiche-probe] version={} ALPN=h3 local={} peer={} sni={} authority={}:{}",
        env!("CARGO_PKG_VERSION"),
        local_addr,
        peer_addr,
        args.fqdn,
        args.fqdn,
        args.port
    );
    if args.cert_hash.is_none() {
        eprintln!("[WARNING] no --cert-hash supplied: peer certificate is NOT pinned");
    }
    eprintln!(
        "[WARNING] quiche Config exposes no verify callback: BoringSSL CA/chain validation is disabled; \
         this probe verifies only the SHA-256 DER hash against the route's pinned certHashes."
    );

    let mut config = quiche::Config::new(quiche::PROTOCOL_VERSION).expect("quiche Config::new");
    config
        .set_application_protos(&[b"h3"])
        .expect("set ALPN h3");
    config.set_max_idle_timeout(10_000);
    config.set_max_recv_udp_payload_size(MAX_DATAGRAM_SIZE);
    config.set_max_send_udp_payload_size(MAX_DATAGRAM_SIZE);
    config.set_disable_active_migration(true);
    // Match the receive windows Chrome advertises in the successful Web Play
    // joins (see docs/FINDINGS.md); the DataModel stream is >2 MB.
    config.set_initial_max_data(15_728_640);
    config.set_initial_max_stream_data_bidi_local(6_291_456);
    config.set_initial_max_stream_data_bidi_remote(6_291_456);
    config.set_initial_max_stream_data_uni(6_291_456);
    config.set_initial_max_streams_bidi(100);
    config.set_initial_max_streams_uni(100);
    config.enable_dgram(true, 100, 100);
    config.verify_peer(false);

    let mut h3_config = quiche::h3::Config::new().expect("h3 Config::new");
    h3_config.set_max_field_section_size(65_536);
    h3_config.set_qpack_max_table_capacity(65_536);
    h3_config.set_qpack_blocked_streams(100);
    h3_config.enable_extended_connect(true);
    // Chrome's Web Play H3 SETTINGS include these draft-02 WebTransport
    // settings (plus the standard ones set above, and H3_DATAGRAM from
    // enable_dgram). See client/page/draftwt.ts.
    h3_config
        .set_additional_settings(vec![
            (0xffd277, 1),
            (0xc671706a, 1),
            (0x2b603743, 1),
            (0x2b603742, 1),
        ])
        .expect("h3 set_additional_settings");

    let mut scid_bytes = [0u8; quiche::MAX_CONN_ID_LEN];
    std::fs::File::open("/dev/urandom")
        .unwrap()
        .read_exact(&mut scid_bytes)
        .unwrap();
    let scid = quiche::ConnectionId::from_ref(&scid_bytes);

    let mut conn = quiche::connect(
        Some(&args.fqdn),
        &scid,
        local_addr,
        peer_addr,
        &mut config,
    )
    .expect("quiche::connect");

    let mut out = vec![0u8; MAX_DATAGRAM_SIZE];
    let (write, send_info) = conn.send(&mut out).expect("initial QUIC send");
    socket.send_to(&out[..write], send_info.to).expect("initial UDP send");
    println!("[quic] sent initial packet ({write} bytes)");

    let mut h3_conn: Option<quiche::h3::Connection> = None;
    let mut req_sent = false;
    let mut got_status = false;
    let mut cert_checked = false;
    let mut buf = vec![0u8; 65_535];
    let started = Instant::now();
    let deadline = started + Duration::from_millis(args.timeout_ms);
    let mut handshake_at: Option<Instant> = None;
    let mut settings_at: Option<Instant> = None;

    while Instant::now() < deadline {
        if conn.is_closed() {
            println!(
                "[quic] connection closed by peer: client={:?} peer={:?}",
                conn.local_error(),
                conn.peer_error()
            );
            break;
        }

        // Flush QUIC output before blocking on input.
        loop {
            match conn.send(&mut out) {
                Ok((n, info)) => {
                    if let Err(e) = socket.send_to(&out[..n], info.to) {
                        eprintln!("[quic] UDP send failed: {e}");
                    }
                }
                Err(quiche::Error::Done) => break,
                Err(e) => {
                    eprintln!("[quic] conn.send error: {e:?}");
                    break;
                }
            }
        }

        let wait = conn
            .timeout()
            .unwrap_or(Duration::from_millis(100))
            .min(Duration::from_millis(100));
        socket.set_read_timeout(Some(wait)).unwrap();

        match socket.recv_from(&mut buf) {
            Ok((len, from)) => {
                let recv_info = quiche::RecvInfo { from, to: local_addr };
                match conn.recv(&mut buf[..len], recv_info) {
                    Ok(read) => {
                        if args.verbose {
                            println!("[quic] received {len} bytes ({read} processed)");
                        }
                    }
                    Err(quiche::Error::Done) => {}
                    Err(e) => {
                        eprintln!("[quic] conn.recv error: {e:?}");
                    }
                }
            }
            Err(ref e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                conn.on_timeout();
            }
            Err(e) => {
                eprintln!("[quic] UDP recv error: {e}");
                conn.on_timeout();
            }
        }

        if conn.is_established() {
            if handshake_at.is_none() {
                handshake_at = Some(Instant::now());
                println!(
                    "[quic] HANDSHAKE ESTABLISHED after {:?} (library {})",
                    handshake_at.unwrap().duration_since(started),
                    env!("CARGO_PKG_VERSION")
                );
            }
            if !cert_checked {
                cert_checked = true;
                if let Err(e) = check_pinned_cert(&conn, args.cert_hash.as_deref()) {
                    eprintln!("[cert] FATAL: {e}");
                    std::process::exit(2);
                }
            }

            if h3_conn.is_none() {
                match quiche::h3::Connection::with_transport(&mut conn, &h3_config) {
                    Ok(h3) => {
                        h3_conn = Some(h3);
                        settings_at = Some(Instant::now());
                        println!("[h3] local SETTINGS sent on control stream");
                    }
                    Err(e) => {
                        eprintln!("[h3] with_transport failed: {e:?}");
                        break;
                    }
                }
            }

            if let Some(h3) = &mut h3_conn {
                if settings_at.is_some() {
                    if let Some(raw) = h3.peer_settings_raw() {
                        println!("[h3] peer SETTINGS: {:?}", raw);
                    }
                }

                if !req_sent {
                    let authority = format!("{}:{}", args.fqdn, args.port);
                    let req = vec![
                        quiche::h3::Header::new(b":method", b"CONNECT"),
                        quiche::h3::Header::new(b":scheme", b"https"),
                        quiche::h3::Header::new(b":authority", authority.as_bytes()),
                        quiche::h3::Header::new(b":path", b"/wt"),
                        quiche::h3::Header::new(b":protocol", b"webtransport"),
                        quiche::h3::Header::new(b"sec-webtransport-http3-draft02", b"1"),
                        quiche::h3::Header::new(
                            b"x-rbx-capabilities",
                            args.caps.as_bytes(),
                        ),
                    ];
                    match h3.send_request(&mut conn, &req, false) {
                        Ok(stream) => {
                            println!("[h3] extended CONNECT sent on stream {stream}: {req:?}");
                            req_sent = true;
                        }
                        Err(quiche::h3::Error::StreamBlocked) => {
                            // retry next iteration once the stream is writable
                        }
                        Err(e) => {
                            eprintln!("[h3] send_request failed: {e:?}");
                            break;
                        }
                    }
                }

                loop {
                    match h3.poll(&mut conn) {
                        Ok((stream, ev)) => {
                            println!("[h3 event] stream={stream} {ev:?}");
                            if let quiche::h3::Event::Headers { list, .. } = ev {
                                let fields: Vec<(String, String)> = list
                                    .iter()
                                    .map(|h| {
                                        (
                                            String::from_utf8_lossy(h.name()).into_owned(),
                                            String::from_utf8_lossy(h.value()).into_owned(),
                                        )
                                    })
                                    .collect();
                                println!("[h3 response] stream={stream} headers={fields:?}");
                                if let Some((_, status)) = fields
                                    .iter()
                                    .find(|(name, _)| name == ":status")
                                {
                                    println!("[RESULT] CONNECT :status = {status}");
                                    got_status = true;
                                }
                            }
                        }
                        Err(quiche::h3::Error::Done) => break,
                        Err(e) => {
                            eprintln!("[h3] poll error: {e:?}");
                            break;
                        }
                    }
                }
            }
        }

        if got_status {
            // Give the server a moment to flush any trailing headers/frames.
            std::thread::sleep(Duration::from_millis(200));
            break;
        }
    }

    println!(
        "[summary] handshake={} h3={} connect_sent={} status_received={} closed={}",
        handshake_at.is_some(),
        h3_conn.is_some(),
        req_sent,
        got_status,
        conn.is_closed()
    );
    println!("[summary] quiche stats: {:?}", conn.stats());

    let mut stderr = std::io::stderr();
    let _ = writeln!(stderr, "[quiche-probe] done");
}
