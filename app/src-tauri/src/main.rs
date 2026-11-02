// raknet-client — desktop shell.
//
// There is no frontend bundle to speak of: the window loads the page that
// rkhost serves on loopback, and that page is the one that talks to the engine
// worker. Serving it from our own origin is what removes the extension, the
// declarativeNetRequest redirect, the CSP rewrite and the Local Network Access
// flag — a page on 127.0.0.1 opening a socket on 127.0.0.1 is local to local.
//
// usage:
//   raknet-client --place 13569002876 \
//                 --wasm host/extension/WebClient.patched.wasm \
//                 --engine-dir ~/.local/share/raknet-client/engine

use std::path::{Path, PathBuf};

use tauri::{WebviewUrl, WebviewWindowBuilder};

struct Args {
    opts: rkhost::Options,
    place: Option<u64>,
}

fn main() {
    // WebKitGTK needs two things that Chrome gives for free:
    //   - SharedArrayBuffer is gated behind a JSC option here, and
    //   - it only understands the original `require-corp` COEP value.
    // Both are read by the web process, which inherits this environment.
    if std::env::var_os("JSC_useSharedArrayBuffer").is_none() {
        std::env::set_var("JSC_useSharedArrayBuffer", "1");
    }
    if std::env::var_os("RKHOST_COEP").is_none() {
        std::env::set_var("RKHOST_COEP", "require-corp");
    }

    let Args { opts, place } = parse_args();

    // Bind before the window exists: a port clash should be an error on stderr,
    // not a blank window.
    let running = rkhost::start(opts);
    let url = running.play_url(place);

    println!("window {url}");
    if !running.has_cookie() {
        println!("no cookie yet — the page will prompt for .ROBLOSECURITY");
    }

    tauri::Builder::default()
        .setup(move |app| {
            let url: tauri::Url = url.parse()?;
            WebviewWindowBuilder::new(app, "main", WebviewUrl::External(url))
                .title("RakNet Client")
                .inner_size(1280.0, 800.0)
                .min_inner_size(800.0, 600.0)
                .build()?;
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("tauri runtime");
}

fn parse_args() -> Args {
    let mut place = None;

    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut rest = Vec::new();
    let mut i = 0;
    while i < argv.len() {
        if argv[i] == "--place" || argv[i] == "--place-id" {
            place = argv.get(i + 1).and_then(|v| v.parse().ok());
            i += 2;
        } else {
            rest.push(argv[i].clone());
            i += 1;
        }
    }
    let mut opts = rkhost::Options::from_args(rest);

    // Defaults are relative to the repo, but the binary may be launched from
    // anywhere. Walk up from cwd and from the executable until host/index.html
    // turns up.
    if !opts.root.join("index.html").is_file() {
        if let Some(p) = locate("host/index.html") {
            if let Some(dir) = p.parent() {
                opts.root = dir.to_path_buf();
            }
        }
    }
    Args { opts, place }
}

fn locate(rel: &str) -> Option<PathBuf> {
    let starts = [
        std::env::current_dir().ok(),
        std::env::current_exe()
            .ok()
            .and_then(|e| e.parent().map(Path::to_path_buf)),
    ];
    starts
        .into_iter()
        .flatten()
        .find_map(|start| find_up(&start, rel))
}

fn find_up(start: &Path, rel: &str) -> Option<PathBuf> {
    let mut cur = Some(start);
    while let Some(dir) = cur {
        let candidate = dir.join(rel);
        if candidate.exists() {
            return Some(candidate);
        }
        cur = dir.parent();
    }
    None
}
