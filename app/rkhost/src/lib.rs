// rkhost — local host for the RakNet-spliced Roblox web client.
//
// Library form: the Tauri shell and the plain CLI both start the same server.
//
//   1. serve the page ourselves, so there is no extension, no CSP to strip and
//      no module redirect -- /engine/WebClient.wasm is simply the patched file;
//   2. proxy Roblox APIs with the account cookie attached server-side, because
//      a page served from 127.0.0.1 is cross-origin to roblox.com;
//   3. own the real UDP socket the patched module cannot open, and pump it to
//      the page over a WebSocket (frame = one raw datagram).
//
// The page is a *local* page, so its WebSocket to another loopback port is not
// the public->private request a browser's Local Network Access blocks.

use std::io::Read;
use std::net::{SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::json;

pub const DEFAULT_PORT: u16 = 8765;
pub const DEFAULT_BRIDGE_PORT: u16 = 37999;

const NATIVE_UA: &str = "Roblox/WinInet";
const WEB_UA: &str = "Roblox/Web Unknown (8192MB; 1366x768; 96x96; 1366x768; ; Linux) \
Browser/Chrome/155.0.0.0 RobloxApp/0.745.0.7451145 (GlobalDist; RobloxDirectDownload)";

struct Cfg {
    root: PathBuf,
    engine_dir: PathBuf,
    wasm: Option<PathBuf>,
    cookie: Mutex<String>,
    cdn_base: String,
    /// UDMUX address the join response named, for RakPeer::Connect.
    peer: Mutex<Option<(String, u16)>>,
    /// Last status snapshot the page posted. The webview has no console we can
    /// read, so the page reports itself here.
    client: Mutex<Option<String>>,
    /// RUPP TLV1 token the join script names; the server drops datagrams that
    /// do not stamp it.
    rupp_token: Mutex<Option<String>>,
    /// RUPP TLV2 target: the private RCC address and the NetStack port the edge
    /// routes on. Not the same as the public UDMUX port.
    rcc: Mutex<Option<(String, u16)>>,
    /// Port subtype-2 datagrams must advertise: ServerConnections[0].Port.
    /// The RCC drops established-session datagrams that name NetStackPort.
    data_port: Mutex<Option<u16>>,
}

/// Everything the server needs. Build it with `Options::default()` and mutate,
/// or let `Options::from_args` parse the CLI form.
#[derive(Clone, Debug)]
pub struct Options {
    pub port: u16,
    pub bridge_port: u16,
    pub root: PathBuf,
    pub engine_dir: PathBuf,
    pub wasm: Option<PathBuf>,
    pub cookie: Option<String>,
    pub cookie_file: Option<PathBuf>,
    pub cdn_base: Option<String>,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            port: DEFAULT_PORT,
            bridge_port: DEFAULT_BRIDGE_PORT,
            root: PathBuf::from("host"),
            engine_dir: PathBuf::from("/tmp/opencode/rw"),
            wasm: None,
            cookie: None,
            cookie_file: None,
            cdn_base: None,
        }
    }
}

impl Options {
    /// Same flags the old binary took, so scripts and docs keep working.
    pub fn from_args<I: IntoIterator<Item = String>>(args: I) -> Self {
        let mut o = Options::default();
        let mut it = args.into_iter();
        while let Some(a) = it.next() {
            match a.as_str() {
                "--port" => o.port = it.next().and_then(|v| v.parse().ok()).unwrap_or(o.port),
                "--bridge-port" => {
                    o.bridge_port = it.next().and_then(|v| v.parse().ok()).unwrap_or(o.bridge_port)
                }
                "--root" => o.root = PathBuf::from(it.next().unwrap_or_default()),
                "--engine-dir" => o.engine_dir = PathBuf::from(it.next().unwrap_or_default()),
                "--wasm" => o.wasm = it.next().map(PathBuf::from),
                "--cookie" => o.cookie_file = it.next().map(PathBuf::from),
                "--cookie-value" => o.cookie = it.next(),
                "--cdn-base" => o.cdn_base = it.next(),
                other => eprintln!("unknown arg {other}"),
            }
        }
        o
    }

    fn into_cfg(self) -> Cfg {
        let cookie = self.cookie.or_else(|| {
            self.cookie_file
                .and_then(|p| std::fs::read_to_string(p).ok())
                .map(|s| s.trim().to_string())
        });
        let cdn_base = self.cdn_base.unwrap_or_else(|| {
            let version = self
                .engine_dir
                .file_name()
                .map(|s| s.to_string_lossy().to_string())
                .unwrap_or_default();
            format!("https://setup.rbxcdn.com/webclient/{version}")
        });
        Cfg {
            root: self.root,
            engine_dir: self.engine_dir,
            wasm: self.wasm,
            cookie: Mutex::new(cookie.unwrap_or_default()),
            cdn_base,
            peer: Mutex::new(None),
            client: Mutex::new(None),
            rupp_token: Mutex::new(None),
            rcc: Mutex::new(None),
            data_port: Mutex::new(None),
        }
    }
}

/// A started server. Dropping it does not stop the threads; they live for the
/// process, which is what both front ends want.
pub struct Running {
    pub http_port: u16,
    pub bridge_port: u16,
    cfg: Arc<Cfg>,
}

impl Running {
    pub fn page_url(&self) -> String {
        format!("http://127.0.0.1:{}/", self.http_port)
    }

    /// The page reads `placeId` from its own query string.
    pub fn play_url(&self, place_id: Option<u64>) -> String {
        match place_id {
            Some(p) => format!("{}?placeId={p}", self.page_url()),
            None => self.page_url(),
        }
    }

    pub fn has_cookie(&self) -> bool {
        !self.cfg.cookie.lock().unwrap().is_empty()
    }

    pub fn set_cookie(&self, value: &str) {
        *self.cfg.cookie.lock().unwrap() = value.trim().to_string();
    }

    /// The UDMUX endpoint the join response named, once it has been seen.
    pub fn peer(&self) -> Option<(String, u16)> {
        self.cfg.peer.lock().unwrap().clone()
    }

    /// Last JSON snapshot posted by the page (ring stats, engine console tail).
    pub fn client_report(&self) -> Option<String> {
        self.cfg.client.lock().unwrap().clone()
    }
}

/// Bind both listeners up front, so a port clash surfaces before a window opens.
pub fn start(opts: Options) -> Running {
    let http_port = opts.port;
    let bridge_port = opts.bridge_port;
    let cfg = Arc::new(opts.into_cfg());

    let server = tiny_http::Server::http(("127.0.0.1", http_port)).expect("bind http");
    let bridge = TcpListener::bind(("127.0.0.1", bridge_port)).expect("bind bridge");

    {
        let cfg = Arc::clone(&cfg);
        std::thread::spawn(move || http_serve(cfg, server));
    }
    {
        let cfg = Arc::clone(&cfg);
        std::thread::spawn(move || bridge_accept(cfg, bridge));
    }

    println!("page   http://127.0.0.1:{http_port}/");
    println!("bridge ws://127.0.0.1:{bridge_port}/");

    Running { http_port, bridge_port, cfg }
}

/// Headless behaviour: start and block forever.
pub fn serve(opts: Options) -> ! {
    let _running = start(opts);
    loop {
        std::thread::park();
    }
}

// ------------------------------------------------------------------ http ----

fn http_serve(cfg: Arc<Cfg>, server: tiny_http::Server) {
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
    if path == "/__log" {
        let mut body = String::new();
        req.as_reader().read_to_string(&mut body)?;
        if let Ok(lines) = serde_json::from_str::<Vec<String>>(&body) {
            for line in lines {
                println!("[page] {line}");
            }
        }
        return send_json(req, 200, json!({ "ok": true }));
    }
    if path == "/__client" {
        if method == tiny_http::Method::Post {
            let mut body = String::new();
            req.as_reader().read_to_string(&mut body)?;
            *cfg.client.lock().unwrap() = Some(body);
            return send_json(req, 200, json!({ "ok": true }));
        }
        let v = cfg.client.lock().unwrap().clone();
        return send(req, 200, "application/json", v.unwrap_or_else(|| "{}".into()).into_bytes());
    }
    if path == "/__peer" {
        let peer = cfg.peer.lock().unwrap().clone();
        return match peer {
            Some((ip, port)) => {
                let tok = cfg.rupp_token.lock().unwrap().clone();
                let rcc = cfg.rcc.lock().unwrap().clone();
                send_json(req, 200, json!({
                    "ip": ip,
                    "port": port,
                    "token": tok,
                    "rcc": rcc.as_ref().map(|(a, _)| a.clone()),
                    "nsport": rcc.map(|(_, p)| p),
                    "sport": *cfg.data_port.lock().unwrap(),
                }))
            }
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

/// The engine creates its workers by pointing them straight at WebClient.js
/// ("Redirecting WebWorker to primary script"), so stub.js never runs inside
/// them and proxy-hook.js would never load there. Prepend it where the engine
/// glue is served; the page context skips it because importScripts is absent.
const WORKER_PRELUDE: &str = ";(function(){try{if(typeof importScripts==='function')importScripts(location.origin+'/proxy-hook.js')}catch(e){}})();\n";

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
        if rel == "WebClient.js" {
            let mut with_prelude = WORKER_PRELUDE.as_bytes().to_vec();
            with_prelude.extend_from_slice(&body);
            return send(req, 200, "text/javascript", with_prelude);
        }
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

    // gamejoin hands out a session that matches the client that asked: a web UA
    // gets a web/QUIC-shaped one whose RakNet edge port never answers. Ask the
    // way the native client does.
    let ua = if url.contains("gamejoin.roblox.com") { NATIVE_UA } else { WEB_UA };
    let mut call = ureq::request(req.method().as_str(), url)
        .set("User-Agent", ua)
        .set("Accept-Encoding", "identity")
        .set("Referer", "https://play.roblox.com/")
        .set("rbx-user-agent", ua);
    if url.contains("gamejoin.roblox.com") {
        call = call.set("Requester", "Client");
    }
    if let Some(ct) = req.headers().iter().find(|h| h.field.as_str().as_str().eq_ignore_ascii_case("content-type")) {
        call = call.set("Content-Type", ct.value.as_str());
    }
    let cookie = cfg.cookie.lock().unwrap().clone();
    if !cookie.is_empty() {
        call = call.set("Cookie", &format!(".ROBLOSECURITY={cookie}"));
    }

    println!("proxy {} {}", req.method(), url);
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
    // Keep the last join payload for inspection.
    if data.windows(11).any(|w| w == b"MachineAddr") {
        let _ = std::fs::write("/tmp/opencode/join.json", data);
        println!("join script captured ({} B)", data.len());
    }

    let Ok(doc) = serde_json::from_slice::<serde_json::Value>(data) else {
        return;
    };
    // The response may nest everything under joinScript, or be the script itself.
    let js = doc.get("joinScript").unwrap_or(&doc);

    // MachineAddress is the datacenter-internal address of the game server and
    // is not routable from here. The public entry point is the UDMUX endpoint,
    // and that is what the real client dials.
    let udmux = js
        .get("UdmuxEndpoints")
        .and_then(|v| v.as_array())
        .and_then(|a| a.first());

    let picked = udmux
        .and_then(|u| {
            let ip = u.get("Address")?.as_str()?.to_string();
            let port = u.get("Port")?.as_u64()? as u16;
            Some((ip, port))
        })
        .or_else(|| {
            let ip = js.get("MachineAddress")?.as_str()?.to_string();
            let port = js.get("ServerPort")?.as_u64()? as u16;
            Some((ip, port))
        });

    // RUPP TLV2 must name the RCC (the private datacenter address) and the
    // NetStack port. Substituting the server/UDMUX port makes the edge drop
    // the handshake without a reply.
    let rcc_ip = js.get("MachineAddress").and_then(|v| v.as_str()).map(|s| s.to_string())
        .or_else(|| {
            js.get("ServerConnections").and_then(|v| v.as_array()).and_then(|a| a.first())
                .and_then(|c| c.get("Address")).and_then(|v| v.as_str()).map(|s| s.to_string())
        });
    let srv_port = js.get("ServerConnections").and_then(|v| v.as_array())
        .and_then(|a| a.first()).and_then(|c| c.get("Port")).and_then(|v| v.as_u64())
        .or_else(|| js.get("ServerPort").and_then(|v| v.as_u64()));
    if let Some(p) = srv_port {
        *cfg.data_port.lock().unwrap() = Some(p as u16);
    }

    if let (Some(ip), Some(port)) = (rcc_ip, js.get("NetStackPort").and_then(|v| v.as_u64())) {
        let mut slot = cfg.rcc.lock().unwrap();
        let want = (ip, port as u16);
        if slot.as_ref() != Some(&want) {
            println!("rcc from join response: {}:{} (netstack)", want.0, want.1);
            *slot = Some(want);
        }
    }

    // The web session hands this over as 32 hex chars; the native-shaped one
    // as base64 of the same 16 bytes. Normalise to hex either way.
    if let Some(tok) = js.get("NetStackTokenValue").and_then(|v| v.as_str()) {
        let hex = if tok.len() == 32 && tok.bytes().all(|b| b.is_ascii_hexdigit()) {
            Some(tok.to_ascii_lowercase())
        } else {
            b64_16(tok).map(|b| b.iter().map(|x| format!("{x:02x}")).collect::<String>())
        };
        if let Some(hex) = hex {
            let mut slot = cfg.rupp_token.lock().unwrap();
            if slot.as_deref() != Some(hex.as_str()) {
                println!("rupp token from join response: {hex}");
                *slot = Some(hex);
            }
        }
    }

    if let Some((ip, port)) = picked {
        if port != 0 {
            let mut peer = cfg.peer.lock().unwrap();
            if peer.as_ref() != Some(&(ip.clone(), port)) {
                let via = if udmux.is_some() { "udmux" } else { "machine" };
                println!("peer from join response ({via}): {ip}:{port}");
                *peer = Some((ip, port));
            }
        }
    }
}

/// Minimal base64 decoder for the 16-byte RUPP token (standard alphabet, with
/// or without padding). Returns None unless it decodes to exactly 16 bytes.
fn b64_16(s: &str) -> Option<[u8; 16]> {
    let mut out = Vec::with_capacity(18);
    let mut acc: u32 = 0;
    let mut bits = 0u32;
    for ch in s.bytes() {
        let v = match ch {
            b'A'..=b'Z' => ch - b'A',
            b'a'..=b'z' => ch - b'a' + 26,
            b'0'..=b'9' => ch - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            b'=' => break,
            _ => return None,
        };
        acc = (acc << 6) | v as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    if out.len() != 16 {
        return None;
    }
    let mut arr = [0u8; 16];
    arr.copy_from_slice(&out);
    Some(arr)
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

/// Chrome wants `credentialless` (blob workers survive it); WebKit only knows
/// `require-corp`. Selectable so both can be measured without a rebuild.
fn coep() -> &'static str {
    match std::env::var("RKHOST_COEP").ok().as_deref() {
        Some("require-corp") | Some("require") => "require-corp",
        Some("none") => "unsafe-none",
        _ => "credentialless",
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
        ("cross-origin-embedder-policy", coep()),
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

fn bridge_accept(cfg: Arc<Cfg>, listener: TcpListener) {
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
    let mut hist: std::collections::BTreeMap<u8, u32> = std::collections::BTreeMap::new();

    loop {
        match ws.read() {
            Ok(tungstenite::Message::Binary(buf)) => {
                if let Some(d) = dest {
                    let _ = udp.send_to(&buf, d);
                    if stats.0 < 6 {
                        let head: String = buf.iter().take(48).map(|b| format!("{b:02x}")).collect::<Vec<_>>().join(" ");
                        println!("udp tx {} B -> {} head=[{}]", buf.len(), d, head);
                    }
                    // Classify by RUPP payload id so a retransmit storm is visible.
                    let id = if buf.len() > 4 && buf[0] == 1 && buf[1] == 0 {
                        let total = ((buf[2] as usize) << 8) | buf[3] as usize;
                        buf.get(total).copied().unwrap_or(0)
                    } else {
                        buf.first().copied().unwrap_or(0)
                    };
                    if id == 0x78 && !hist.contains_key(&0x78) {
                        let total = ((buf[2] as usize) << 8) | buf[3] as usize;
                        let body = &buf[total.min(buf.len())..];
                        let hex: String = body.iter().take(64).map(|b| format!("{b:02x}")).collect::<Vec<_>>().join(" ");
                        println!("first Req2: total={} payload={} B head=[{}]", total, body.len(), hex);
                    }
                    *hist.entry(id).or_insert(0) += 1;
                    if stats.0 % 500 == 0 {
                        let keys: Vec<String> = hist.iter().map(|(k, v)| format!("{k:02x}:{v}")).collect();
                        println!("udp ids [{}]", keys.join(" "));
                    }
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
            if stats.1 < 12 {
                let head: String = rx[..n.min(40)].iter().map(|b| format!("{b:02x}")).collect::<Vec<_>>().join(" ");
                println!("udp rx {} B <- {} head=[{}]", n, from, head);
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
