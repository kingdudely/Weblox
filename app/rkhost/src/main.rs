// rkhost — local host for the RakNet-spliced Roblox web client.
//
// Three jobs in one process, replacing the python server + node bridge:
//
//   1. serve the page ourselves, so there is no extension, no CSP to strip and
//      no module redirect -- /engine/WebClient.wasm is simply the patched file;
//   2. proxy Roblox APIs with the account cookie attached server-side, because
//      a page served from 127.0.0.1 is cross-origin to roblox.com;
//   3. own the real UDP socket the patched module cannot open, and pump it to
//      the page over a WebSocket (frame = one raw datagram).
//
// The page is a *local* page, so its WebSocket to another loopback port is not
// the public->private request Chrome's Local Network Access blocks.
//
// usage:
//   rkhost --wasm ../host/extension/WebClient.patched.wasm \
//          --engine-dir /tmp/opencode/rw --cookie ~/cookie.txt [--port 8765]

use std::io::Read;
use std::net::{SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::json;

const WEB_UA: &str = "Roblox/Web Unknown (8192MB; 1366x768; 96x96; 1366x768; ; Linux) \
Browser/Chrome/155.0.0.0 RobloxApp/0.745.0.7451145 (GlobalDist; RobloxDirectDownload)";

#[derive(Default)]
struct Cfg {
    root: PathBuf,
    engine_dir: PathBuf,
    wasm: Option<PathBuf>,
    cookie: Mutex<String>,
    cdn_base: String,
    /// UDMUX address the join response named, for RakPeer::Connect.
    peer: Mutex<Option<(String, u16)>>,
}

fn main() {
    let mut port = 8765u16;
    let mut bridge = 37999u16;
    let mut cfg = Cfg {
        root: PathBuf::from("host"),
        engine_dir: PathBuf::from("/tmp/opencode/rw"),
        ..Default::default()
    };
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--port" => port = args.next().and_then(|v| v.parse().ok()).unwrap_or(port),
            "--bridge-port" => bridge = args.next().and_then(|v| v.parse().ok()).unwrap_or(bridge),
            "--root" => cfg.root = PathBuf::from(args.next().unwrap_or_default()),
            "--engine-dir" => cfg.engine_dir = PathBuf::from(args.next().unwrap_or_default()),
            "--wasm" => cfg.wasm = args.next().map(PathBuf::from),
            "--cookie" => {
                let p = args.next().unwrap_or_default();
                let val = std::fs::read_to_string(p).unwrap_or_default();
                *cfg.cookie.lock().unwrap() = val.trim().to_string();
            }
            "--cdn-base" => cfg.cdn_base = args.next().unwrap_or_default(),
            "--help" | "-h" => {
                println!("rkhost --wasm W --engine-dir D [--cookie C] [--port 8765] [--bridge-port 37999]");
                return;
            }
            other => eprintln!("unknown arg {other}"),
        }
    }
    if cfg.cdn_base.is_empty() {
        let version = cfg
            .engine_dir
            .file_name()
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_default();
        cfg.cdn_base = format!("https://setup.rbxcdn.com/webclient/{version}");
    }

    let cfg = Arc::new(cfg);
    let bcfg = Arc::clone(&cfg);
    std::thread::spawn(move || bridge_loop(bcfg, bridge));

    println!("page  http://127.0.0.1:{port}/");
    println!("bridge ws://127.0.0.1:{bridge}/");
    http_loop(cfg, port);
}

// ------------------------------------------------------------------ http ----

fn http_loop(cfg: Arc<Cfg>, port: u16) {
    let server = tiny_http::Server::http(("127.0.0.1", port)).expect("bind http");
    for req in server.incoming_requests() {
        let cfg = Arc::clone(&cfg);
        std::thread::spawn(move || {
            if let Err(e) = handle(cfg, req) {
                eprintln!("request failed: {e}");
            }
        });
    }
}

fn handle(cfg: Arc<Cfg>, mut req: tiny_http::Request) -> std::io::Result<()> {
    let path = req.url().split('?').next().unwrap_or("/").to_string();
    let method = req.method().clone();

    if path == "/__status" {
        let has = !cfg.cookie.lock().unwrap().is_empty();
        return send_json(req, 200, json!({ "cookie": has }));
    }
    if path == "/__peer" {
        let peer = cfg.peer.lock().unwrap().clone();
        return match peer {
            Some((ip, port)) => send_json(req, 200, json!({ "ip": ip, "port": port })),
            None => send_json(req, 200, json!({})),
        };
    }
    if path == "/__cookie" {
        let mut body = String::new();
        req.as_reader().read_to_string(&mut body)?;
        let v = body.trim().to_string();
        // Cheap sanity check: a real cookie is long and base64-ish.
        if v.len() < 200 {
            return send_json(req, 400, json!({ "ok": false }));
        }
        *cfg.cookie.lock().unwrap() = v;
        println!("cookie accepted ({} chars)", body.trim().len());
        return send_json(req, 200, json!({ "ok": true }));
    }

    if let Some(rel) = path.strip_prefix("/engine/") {
        return engine(cfg, req, rel);
    }
    if let Some(rest) = path.strip_prefix("/proxy/") {
        let (host, sub) = rest.split_once('/').unwrap_or((rest, ""));
        let query = req.url().split_once('?').map(|(_, q)| q).unwrap_or("");
        let target = format!("https://{host}/{sub}{}", if query.is_empty() { String::new() } else { format!("?{query}") });
        return proxy(cfg, req, &target);
    }

    let file = match path.as_str() {
        "/" | "/index.html" => cfg.root.join("index.html"),
        "/ring.js" => cfg.root.join("ring.js"),
        "/proxy-hook.js" => cfg.root.join("proxy-hook.js"),
        "/stub.js" => cfg.root.join("stub.js"),
        _ => PathBuf::new(),
    };
    if file.is_file() && method == tiny_http::Method::Get {
        let body = std::fs::read(&file)?;
        return send(req, 200, ctype(&file), body);
    }
    send(req, 404, "text/plain", b"not found".to_vec())
}

fn engine(cfg: Arc<Cfg>, req: tiny_http::Request, rel: &str) -> std::io::Result<()> {
    let rel = rel.split('?').next().unwrap_or(rel);
    if rel == "WebClient.wasm" {
        if let Some(w) = &cfg.wasm {
            println!("serving patched module");
            let body = std::fs::read(w)?;
            return send(req, 200, "application/wasm", body);
        }
    }
    let local = cfg.engine_dir.join(rel);
    if local.is_file() {
        let body = std::fs::read(&local)?;
        return send(req, 200, ctype(&local), body);
    }
    let url = format!("{}/{}", cfg.cdn_base.trim_end_matches('/'), rel);
    proxy(cfg, req, &url)
}

fn proxy(cfg: Arc<Cfg>, mut req: tiny_http::Request, url: &str) -> std::io::Result<()> {
    let mut body = Vec::new();
    if req.method() == &tiny_http::Method::Post {
        req.as_reader().read_to_end(&mut body)?;
    }

    let mut call = ureq::request(req.method().as_str(), url)
        .set("User-Agent", WEB_UA)
        .set("Accept-Encoding", "identity")
        .set("Referer", "https://play.roblox.com/")
        .set("rbx-user-agent", WEB_UA);
    if let Some(ct) = req.headers().iter().find(|h| h.field.as_str().as_str().eq_ignore_ascii_case("content-type")) {
        call = call.set("Content-Type", ct.value.as_str());
    }
    let cookie = cfg.cookie.lock().unwrap().clone();
    if !cookie.is_empty() {
        call = call.set("Cookie", &format!(".ROBLOSECURITY={cookie}"));
    }

    let resp = if body.is_empty() { call.call() } else { call.send_bytes(&body) };
    let (status, ctype, mut data) = match resp {
        Ok(r) => {
            let ct = r.header("content-type").unwrap_or("application/octet-stream").to_string();
            let mut buf = Vec::new();
            r.into_reader().read_to_end(&mut buf)?;
            (200u16, ct, buf)
        }
        Err(ureq::Error::Status(code, r)) => {
            let ct = r.header("content-type").unwrap_or("application/octet-stream").to_string();
            let mut buf = Vec::new();
            r.into_reader().read_to_end(&mut buf)?;
            (code, ct, buf)
        }
        Err(e) => {
            eprintln!("upstream {url} failed: {e}");
            return send(req, 502, "text/plain", e.to_string().into_bytes());
        }
    };

    // Force the engine onto RakNet instead of the dummy RbxTransport client.
    if url.contains("/v2/settings/application/WebClientApp") && data.first() == Some(&b'{') {
        if let Ok(mut doc) = serde_json::from_slice::<serde_json::Value>(&data) {
            if let Some(app) = doc.get_mut("applicationSettings").and_then(|v| v.as_object_mut()) {
                app.insert("FFlagUseRbxTransportClient3".into(), json!("False"));
                app.insert("FFlagUseRbxTransportServer".into(), json!("False"));
                data = serde_json::to_vec(&doc).unwrap_or(data);
                println!("settings: RakNet forced on");
            }
        }
    }

    // The join response names the UDMUX endpoint; that is the address the
    // engine's own RakNet peer has to be pointed at.
    sniff_peer(&cfg, &data);
    send(req, status, &ctype, data)
}

fn sniff_peer(cfg: &Cfg, data: &[u8]) {
    let Ok(text) = std::str::from_utf8(data) else { return };
    let addr = grab(text, "\"MachineAddress\"");
    let port = grab_number(text, "\"ServerPort\"").or_else(|| grab_number(text, "\"UdmuxPort\""));
    if let (Some(ip), Some(p)) = (addr, port) {
        if let Ok(p) = p.parse::<u16>() {
            let mut peer = cfg.peer.lock().unwrap();
            if peer.as_ref() != Some(&(ip.clone(), p)) {
                println!("peer from join response: {ip}:{p}");
                *peer = Some((ip, p));
            }
        }
    }
}

fn grab(text: &str, key: &str) -> Option<String> {
    let at = text.find(key)? + key.len();
    let rest = &text[at..];
    let rest = rest.trim_start().strip_prefix(':')?.trim_start();
    let rest = rest.strip_prefix('"')?;
    let end = rest.find('"')?;
    Some(rest[..end].to_string())
}

fn grab_number(text: &str, key: &str) -> Option<String> {
    let at = text.find(key)? + key.len();
    let rest = text[at..].trim_start().strip_prefix(':')?.trim_start();
    let end = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
    Some(rest[..end].to_string())
}

fn ctype(p: &Path) -> &'static str {
    match p.extension().and_then(|e| e.to_str()).unwrap_or("") {
        "js" => "text/javascript",
        "wasm" => "application/wasm",
        "json" => "application/json",
        "html" => "text/html; charset=utf-8",
        "css" => "text/css",
        _ => "application/octet-stream",
    }
}

fn send(req: tiny_http::Request, status: u16, ctype: &str, body: Vec<u8>) -> std::io::Result<()> {
    use tiny_http::{Header, Response};
    let mut resp = Response::from_data(body)
        .with_status_code(status)
        .with_header(Header::from_bytes("content-type", ctype).unwrap());
    // Required for SharedArrayBuffer: the engine's pthread memory is shared.
    for (k, v) in [
        ("cross-origin-opener-policy", "same-origin"),
        ("cross-origin-embedder-policy", "credentialless"),
        ("cross-origin-resource-policy", "same-origin"),
        ("cache-control", "no-store"),
    ] {
        resp = resp.with_header(Header::from_bytes(k, v).unwrap());
    }
    req.respond(resp)
}

fn send_json(req: tiny_http::Request, status: u16, v: serde_json::Value) -> std::io::Result<()> {
    send(req, status, "application/json", serde_json::to_vec(&v).unwrap_or_default())
}

// ----------------------------------------------------------------- bridge ----
// One raw datagram per WebSocket frame; text frames are control JSON
// ({"t":"dest","v":"ip:port"}) so the launcher can retarget the join.

fn bridge_loop(cfg: Arc<Cfg>, port: u16) {
    let listener = TcpListener::bind(("127.0.0.1", port)).expect("bind bridge");
    println!("bridge listening on {port}");
    for stream in listener.incoming().flatten() {
        let cfg = Arc::clone(&cfg);
        std::thread::spawn(move || {
            if let Err(e) = serve_ws(cfg, stream) {
                eprintln!("bridge client ended: {e}");
            }
        });
    }
}

fn serve_ws(cfg: Arc<Cfg>, stream: TcpStream) -> anyhow_like::Result<()> {
    stream.set_read_timeout(Some(Duration::from_millis(5)))?;
    let mut ws = tungstenite::accept(stream).map_err(|e| e.to_string())?;
    let udp = UdpSocket::bind("0.0.0.0:0")?;
    udp.set_read_timeout(Some(Duration::from_millis(5)))?;
    println!("engine connected to bridge");

    let mut dest: Option<SocketAddr> = None;
    let mut rx = [0u8; 2048];
    let mut stats = (0u64, 0u64);

    loop {
        match ws.read() {
            Ok(tungstenite::Message::Binary(buf)) => {
                if let Some(d) = dest {
                    let _ = udp.send_to(&buf, d);
                    stats.0 += 1;
                }
            }
            Ok(tungstenite::Message::Text(txt)) => {
                if let Ok(v) = serde_json::from_str::<serde_json::Value>(&txt) {
                    if v.get("t").and_then(|t| t.as_str()) == Some("dest") {
                        if let Some(s) = v.get("v").and_then(|x| x.as_str()) {
                            if let Ok(a) = s.parse::<SocketAddr>() {
                                println!("destination set -> {a}");
                                dest = Some(a);
                            }
                        }
                    }
                }
            }
            Ok(tungstenite::Message::Close(_)) => break,
            Ok(_) => {}
            Err(tungstenite::Error::Io(ref e))
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => return Err(e.into()),
        }

        if let Ok((n, from)) = udp.recv_from(&mut rx) {
            if dest.is_none() {
                dest = Some(from);
            }
            let _ = ws.send(tungstenite::Message::Binary(rx[..n].to_vec()));
            stats.1 += 1;
        }
        if stats.0 + stats.1 > 0 && (stats.0 + stats.1) % 500 == 0 {
            println!("bridge tx={} rx={}", stats.0, stats.1);
        }
    }
    let _ = &cfg;
    Ok(())
}

// tungstenite's error type is large; a tiny wrapper keeps signatures readable.
mod anyhow_like {
    pub struct Error(String);
    impl std::fmt::Display for Error {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            f.write_str(&self.0)
        }
    }
    impl std::fmt::Debug for Error {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            f.write_str(&self.0)
        }
    }
    impl From<String> for Error {
        fn from(e: String) -> Self {
            Error(e)
        }
    }
    impl From<std::io::Error> for Error {
        fn from(e: std::io::Error) -> Self {
            Error(e.to_string())
        }
    }
    impl From<tungstenite::Error> for Error {
        fn from(e: tungstenite::Error) -> Self {
            Error(e.to_string())
        }
    }
    pub type Result<T> = std::result::Result<T, Error>;
}
