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
    let mut u = url;
    if let Some(r) = u.strip_prefix("rbxasset://") {
        u = r;
    }
    format!("{}{}", base, u.trim_start_matches('/'))
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap_async("env", "js_fetch_file",
        |mut c: Caller<'_, St>, (urlp, dst, want): (i32, i32, i32)| Box::new(async move {
            let url = rstr(&c, urlp as u32);
            let want = want as u32;
            crate::diag::event(format!("fetch {url} want={want}"));
            let Some(path) = local_path(&url) else {
                return Ok(-1i32);
            };

            let serve = |c: &mut Caller<'_, St>, body: &[u8]| {
                let d = c.data().mem.data();
                for (i, b) in body.iter().enumerate() {
                    unsafe { *d[dst as usize + i].get() = *b };
                }
            };

            // local mirror, exact size only
            if let Ok(meta) = std::fs::metadata(&path) {
                if meta.len() == want as u64 {
                    match std::fs::read(&path) {
                        Ok(b) => {
                            serve(&mut c, &b);
                            return Ok(0i32);
                        }
                        Err(_) => return Ok(-1i32),
                    }
                }
            }

            // miss (or stale): pull the CDN copy and cache it
            let base = c.data().base.clone();
            if base.is_empty() {
                return Ok(-1i32);
            }
            let url2 = remote(&url, &base);
            eprintln!("[fs  ] fetch {url2}");
            let body = match curl(&url2, want as u64) {
                Some(b) => b,
                None => return Ok(-1i32),
            };
            if let Some(dir) = std::path::Path::new(&path).parent() {
                let _ = std::fs::create_dir_all(dir);
            }
            if let Ok(mut f) = std::fs::File::create(&path) {
                let _ = f.write_all(&body);
            }
            serve(&mut c, &body);
            Ok(0i32)
        }))?;
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
