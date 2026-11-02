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
    /// Which engine to present the page in. `webkit` is the embedded Tauri
    /// window; `chrome` starts a Chromium app window, which has working pointer
    /// lock and GPU compositing without any page-side compensation.
    browser: Browser,
    /// Window size, which is also the engine's framebuffer size: pixel work
    /// scales with it, and the GPU here is a UHD 620.
    size: (f64, f64),
}

#[derive(Clone, Copy, PartialEq)]
enum Browser {
    Webkit,
    Chrome,
}

fn main() {
    // WebKitGTK gates SharedArrayBuffer behind a JSC option; the web process
    // reads it from this environment. Chromium needs nothing.
    if std::env::var_os("JSC_useSharedArrayBuffer").is_none() {
        std::env::set_var("JSC_useSharedArrayBuffer", "1");
    }

    let Args { opts, place, browser, size } = parse_args();

    // Bind before the window exists: a port clash should be an error on stderr,
    // not a blank window.
    let running = rkhost::start(opts);
    let url = running.play_url(place);

    println!("window {url}");
    if !running.has_cookie() {
        println!("no cookie yet — the page will prompt for .ROBLOSECURITY");
    }

    if browser == Browser::Chrome {
        chrome_window(&url, size);
    }

    tauri::Builder::default()
        .setup(move |app| {
            let url: tauri::Url = url.parse()?;
            let win = WebviewWindowBuilder::new(app, "main", WebviewUrl::External(url))
                .title("RakNet Client")
                .inner_size(size.0, size.1)
                .min_inner_size(800.0, 600.0)
                .build()?;
            // WebKit refuses pointer lock while its window is unfocused
            // ("Pointer lock requires the window to have focus"), and a window
            // that maps without focus never gets it from a click alone on some
            // compositors.
            win.show()?;
            let _ = win.set_focus();
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("tauri runtime");
}

/// Chromium app window: same page, same bridge, none of WebKit's gaps. Pointer
/// lock needs no shim here and the GPU path is the one the engine was built
/// against, so this is the mode to compare against when WebKit misbehaves.
fn chrome_window(url: &str, size: (f64, f64)) -> ! {
    let profile = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::home_dir().map(|h| h.join(".local/share")))
        .unwrap_or_else(|| PathBuf::from("/tmp"))
        .join("dev.zalakami.raknet-client/chrome-profile");
    let _ = std::fs::create_dir_all(&profile);

    let bin = std::env::var("WEBLOX_CHROME").unwrap_or_else(|_| "google-chrome".to_string());
    let status = std::process::Command::new(&bin)
        .arg(format!("--app={url}"))
        .arg(format!("--window-size={},{}", size.0, size.1))
        .arg(format!("--user-data-dir={}", profile.display()))
        .arg("--no-first-run")
        .arg("--no-default-browser-check")
        .arg("--class=RakNetClient")
        .status()
        .unwrap_or_else(|e| {
            eprintln!("could not start {bin}: {e}");
            std::process::exit(1);
        });
    std::process::exit(status.code().unwrap_or(0))
}

fn parse_args() -> Args {
    let mut place = None;
    let mut browser = Browser::Webkit;
    let mut size = (1280.0, 800.0);

    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut rest = Vec::new();
    let mut i = 0;
    while i < argv.len() {
        if argv[i] == "--size" {
            if let Some((w, h)) = argv.get(i + 1).and_then(|v| v.split_once('x')) {
                size = (
                    w.parse().unwrap_or(size.0),
                    h.parse().unwrap_or(size.1),
                );
            }
            i += 2;
        } else if argv[i] == "--browser" {
            browser = match argv.get(i + 1).map(String::as_str) {
                Some("chrome") | Some("chromium") => Browser::Chrome,
                _ => Browser::Webkit,
            };
            i += 2;
        } else if argv[i] == "--place" || argv[i] == "--place-id" {
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
    Args { opts, place, browser, size }
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
