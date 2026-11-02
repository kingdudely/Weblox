// rkhost — static host for the Isolated Web App.
//
// The app is pure JS + wasm: `UDPSocket` is the datagram seam, a controlled
// frame does the authenticated join, and the engine tree is staged into the
// served root by tools/stage_engine.sh. That leaves this process with exactly
// one job — hand out the bundle with the security policy Chrome requires of an
// installed app.
//
// It exists at all because dev-mode install takes an http URL; a signed bundle
// would not need it.

use std::path::{Path, PathBuf};

const DEFAULT_PORT: u16 = 8770;

/// The IWA policy. Chrome refuses to install an app without one, and these are
/// the directives this app actually needs:
///
///   script-src 'self'        the bundle only — the engine scripts are staged in
///   'wasm-unsafe-eval'       WebClient.wasm
///   require-trusted-types    means boot.js installs a default policy
///   connect-src https:       clientsettings and rbxcdn
///   frame-src https:         the roblox.com controlled frame
const CSP: &str = "base-uri 'none'; default-src 'self'; object-src 'none'; \
frame-src 'self' https: blob: data:; connect-src 'self' https: wss: blob: data:; \
script-src 'self' 'wasm-unsafe-eval'; img-src 'self' https: blob: data:; \
media-src 'self' https: blob: data:; font-src 'self' blob: data:; \
style-src 'self' 'unsafe-inline'; require-trusted-types-for 'script'; \
frame-ancestors 'self'";

fn main() {
    let mut port = DEFAULT_PORT;
    let mut root = PathBuf::from("iwa");

    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--port" => port = args.next().and_then(|v| v.parse().ok()).unwrap_or(port),
            "--root" => root = PathBuf::from(args.next().unwrap_or_default()),
            "-h" | "--help" => {
                println!("rkhost [--root iwa] [--port {DEFAULT_PORT}]");
                return;
            }
            other => eprintln!("unknown arg {other}"),
        }
    }

    if !root.is_dir() {
        eprintln!("no app root at {} — stage it first: ./tools/stage_engine.sh", root.display());
        std::process::exit(1);
    }
    println!("serving {} on http://127.0.0.1:{port}/", root.display());
    serve(root, port);
}

fn serve(root: PathBuf, port: u16) -> ! {
    let server = tiny_http::Server::http(("127.0.0.1", port)).expect("bind http");
    for request in server.incoming_requests() {
        let root = root.clone();
        // One thread per request: the engine fetches hundreds of assets at once
        // while the wasm itself is 100 MB, and a serial loop stalls the loader.
        std::thread::spawn(move || {
            if let Err(e) = handle(&root, request) {
                eprintln!("request failed: {e}");
            }
        });
    }
    unreachable!("the accept loop returns only on a fatal server error")
}

fn handle(root: &Path, request: tiny_http::Request) -> std::io::Result<()> {
    let url = request.url().to_string();
    let path = url.split('?').next().unwrap_or("/");

    if request.method() != &tiny_http::Method::Get {
        return respond(request, 405, "text/plain", b"method not allowed".to_vec());
    }

    let relative = path.trim_start_matches('/');
    if relative.split('/').any(|segment| segment == "..") {
        return respond(request, 400, "text/plain", b"bad path".to_vec());
    }

    // The app origin is the bundle root, so "/" is index.html and everything
    // else is a file under it — including the staged engine tree.
    let file = if relative.is_empty() || relative.ends_with('/') {
        root.join(relative).join("index.html")
    } else {
        root.join(relative)
    };

    match std::fs::read(&file) {
        Ok(body) => respond(request, 200, content_type(&file), body),
        // A miss under /engine/ is always a stale mirror, and it is silent in
        // the client, so say it out loud.
        Err(_) if relative.starts_with("engine/") => {
            eprintln!("MISSING {relative} — re-run ./tools/mirror_engine.py --capture");
            respond(request, 404, "text/plain", b"not found".to_vec())
        }
        Err(_) => respond(request, 404, "text/plain", b"not found".to_vec()),
    }
}

fn content_type(path: &Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()).unwrap_or("") {
        "js" => "text/javascript",
        "mjs" => "text/javascript",
        "wasm" => "application/wasm",
        "json" => "application/json",
        "html" => "text/html; charset=utf-8",
        "css" => "text/css",
        "webmanifest" => "application/manifest+json",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "svg" => "image/svg+xml",
        "otf" => "font/otf",
        "ttf" => "font/ttf",
        "mp3" => "audio/mpeg",
        "ogg" => "audio/ogg",
        "xml" => "application/xml",
        "txt" | "lua" => "text/plain",
        _ => "application/octet-stream",
    }
}

fn respond(
    request: tiny_http::Request,
    status: u16,
    content_type: &str,
    body: Vec<u8>,
) -> std::io::Result<()> {
    use tiny_http::{Header, Response};

    let mut response = Response::from_data(body)
        .with_status_code(status)
        .with_header(Header::from_bytes("content-type", content_type).unwrap());

    for (name, value) in [
        ("content-security-policy", CSP),
        // SharedArrayBuffer: the engine's pthread build uses one shared memory.
        ("cross-origin-opener-policy", "same-origin"),
        ("cross-origin-embedder-policy", "require-corp"),
        ("cross-origin-resource-policy", "same-origin"),
        ("cache-control", "no-store"),
    ] {
        response = response.with_header(Header::from_bytes(name, value).unwrap());
    }
    request.respond(response)
}
