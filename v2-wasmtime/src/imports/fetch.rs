//! js_fetch_file: the VFS read hook. The engine hands a URL and a destination
//! buffer; anything under the asset root is served from the mirror, and a
//! missing file is fetched from the CDN and cached back into the mirror.
use std::io::Write;

use anyhow::Result;
use wasmtime::{Caller, Linker};

use crate::state::{rstr, St};

const MAX_FETCH: u64 = 256 << 20;

fn root() -> String {
    std::env::var("WC_CONTENT").unwrap_or_else(|_| "data/content".into())
}

fn local_path(url: &str) -> Option<String> {
    let mut u = url;
    if let Some(rest) = url.strip_prefix("https://").or_else(|| url.strip_prefix("http://")) {
        let end = rest.find('/').unwrap_or(rest.len());
        u = &rest[end..];
    }
    if let Some(r) = u.strip_prefix("rbxasset://") {
        u = r;
    }
    if u.starts_with("data:") || u.starts_with("rbxthumb://") {
        return None;
    }
    let u = u.trim_start_matches('/');
    if u.is_empty() || u.contains("..") {
        return None;
    }
    Some(format!("{}/{u}", root()))
}

fn remote(url: &str, base: &str) -> String {
    if url.starts_with("http://") || url.starts_with("https://") {
        return url.to_string();
    }
    let mut u = url;
    if let Some(r) = u.strip_prefix("rbxasset://") {
        u = r;
    }
    format!("{}{}", base, u.trim_start_matches('/'))
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap_async("env", "js_fetch_file",
        |c: Caller<'_, St>, (urlp, dst, want): (i32, i32, i32)| {
        let mut c = c;
        let fut: Box<dyn std::future::Future<Output = i32> + Send> = Box::new(async move {
            // the page's fetch is a suspending import; yield once so the
            // engine's caller resumes on its fibre instead of running the
            // whole read inside the import frame
            let sp = crate::fibre::store_ptr();
            crate::fibre::sleep(unsafe { &mut *sp }, std::time::Duration::from_micros(200)).await;
            crate::pump_frame();
            let url = rstr(&c, urlp as u32);
            let want = want as u32;
            crate::diag::event(format!("fetch {url} want={want}"));
            let Some(path) = local_path(&url) else {
                eprintln!("[fs  ] no local path {url}");
                return -1;
            };

            let serve = |c: &mut Caller<'_, St>, body: &[u8]| {
                let d = c.data().mem.data();
                unsafe { std::ptr::copy_nonoverlapping(body.as_ptr(), d[dst as usize].get(), body.len()) };
            };

            // local mirror, exact size only
            if let Ok(meta) = std::fs::metadata(&path) {
                eprintln!("[fs  ] hit? {path} size={} want={want}", meta.len());
                if meta.len() == want as u64 {
                    match std::fs::read(&path) {
                        Ok(b) => {
                            serve(&mut c, &b);
                            eprintln!("[fs  ] served {}", b.len());
                            return 0;
                        }
                        Err(_) => return -1,
                    }
                }
            }

            // miss (or stale): pull the CDN copy and cache it
            let base = c.data().base.clone();
            let url2 = remote(&url, &base);
            if !url2.starts_with("http") {
                return -1;
            }
            eprintln!("[fs  ] miss {path}");
            let body = match curl(&url2, want as u64) {
                Some(b) => b,
                None => return -1,
            };
            if let Some(dir) = std::path::Path::new(&path).parent() {
                let _ = std::fs::create_dir_all(dir);
            }
            if let Ok(mut f) = std::fs::File::create(&path) {
                let _ = f.write_all(&body);
            }
            serve(&mut c, &body);
            0
        });
        fut
        })?;
    Ok(())
}

fn curl(url: &str, want: u64) -> Option<Vec<u8>> {
    if want > MAX_FETCH {
        return None;
    }
    let out = std::process::Command::new("curl")
        .args(["-fsSL", "--max-time", "120", url])
        .output()
        .ok()?;
    if !out.status.success() || out.stdout.len() as u64 != want {
        return None;
    }
    Some(out.stdout)
}
