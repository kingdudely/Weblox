//! emscripten_fetch over the native network path.
//!
//! The engine's HTTP goes through Emscripten's fetch API; in the browser that
//! lands in XHR. Here the request is made directly, the response is written
//! back into the fetch object with the same field layout the glue uses, and
//! the guest's own callback is invoked. Requests to roblox.com carry the
//! session cookie, which is what the page's `withCredentials` would supply.
use std::collections::HashMap;
use std::io::Write;

use anyhow::Result;
use wasmtime::{Caller, Linker, Val};

use crate::state::{rbytes, rstr, wbytes, St};

/// `emscripten_fetch_t` field offsets, read off the shipped glue (`an`).
const F_URL: u32 = 8;
const F_DATA: u32 = 12;
const F_NUM: u32 = 16;
const F_OFF: u32 = 24;
const F_TOTAL: u32 = 32;
const F_STATE: u32 = 40;
const F_STATUS: u32 = 42;
const F_TEXT: u32 = 44;
const F_RESPURL: u32 = 200;
const F_ATTR: u32 = 108;
const A_METHOD: u32 = 0;
const A_FLAGS: u32 = 52;
const A_TIMEOUT: u32 = 56;
const A_USER: u32 = 68;
const A_PASS: u32 = 72;
const A_MIME: u32 = 76;
const A_HEADERS: u32 = 80;
const A_ONLOAD: u32 = 36;
const A_ONERROR: u32 = 40;
const A_ONPROGRESS: u32 = 44;
const A_ONSTATE: u32 = 48;
const A_BODY: u32 = 84;
const A_BODYLEN: u32 = 88;

const MAX_BODY: usize = 256 << 20;

fn word(c: &Caller<'_, St>, p: u32) -> u32 {
    let d = c.data().mem.data();
    unsafe { *(d[p as usize].get() as *const u32) }
}

fn put_u64(c: &mut Caller<'_, St>, p: u32, v: u64) {
    wbytes(c, p, &v.to_le_bytes());
}

fn put_u16(c: &mut Caller<'_, St>, p: u32, v: u16) {
    wbytes(c, p, &v.to_le_bytes());
}

/// Invoke a guest callback given its wasm table index.
fn callback(c: &mut Caller<'_, St>, idx: u32, fetch: u32) -> Result<()> {
    let Some(inst) = c.data().inst else { return Ok(()) };
    let t = inst.get_table(&mut *c, "__indirect_function_table");
    let Some(t) = t else { return Ok(()) };
    let Some(wasmtime::Ref::Func(Some(f))) = t.get(&mut *c, idx as u64) else {
        return Ok(());
    };
    let mut out = [];
    f.call(&mut *c, &[Val::I32(fetch as i32)], &mut out)?;
    Ok(())
}

fn host_of(url: &str) -> &str {
    let rest = url.split("://").nth(1).unwrap_or(url);
    let end = rest.find(['/', ':', '?']).unwrap_or(rest.len());
    &rest[..end]
}

fn wants_cookie(url: &str) -> bool {
    let h = host_of(url);
    h == "roblox.com" || h.ends_with(".roblox.com") || h.starts_with("rbxcdn.com")
}

struct Req {
    url: String,
    method: String,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
    timeout: u32,
    mime: String,
    user: String,
    pass: String,
}

fn parse(c: &Caller<'_, St>, fetch: u32) -> Option<Req> {
    let up = word(c, fetch + F_URL);
    if up == 0 {
        return None;
    }
    let url = rstr(c, up);
    if url.is_empty() {
        return None;
    }
    // only http(s) can be serviced here; blob:/data: are handled elsewhere
    if !url.starts_with("http://") && !url.starts_with("https://") {
        return None;
    }
    let a = fetch + F_ATTR;
    let m = word(c, a + A_METHOD);
    let method = if m == 0 { String::new() } else { rstr(c, m) };
    let mut headers = Vec::new();
    let mut hp = word(c, a + A_HEADERS);
    if hp != 0 {
        for _ in 0..64 {
            let n = word(c, hp);
            let v = word(c, hp + 4);
            if n == 0 || v == 0 {
                break;
            }
            headers.push((rstr(c, n), rstr(c, v)));
            hp += 8;
        }
    }
    let bp = word(c, a + A_BODY);
    let bl = word(c, a + A_BODYLEN) as usize;
    let body = if bp != 0 && bl > 0 && bl <= MAX_BODY {
        rbytes(c, bp, bl as u32)
    } else {
        Vec::new()
    };
    let mp = word(c, a + A_MIME);
    let up2 = word(c, a + A_USER);
    let pp = word(c, a + A_PASS);
    Some(Req {
        url,
        method: if method.is_empty() { "GET".into() } else { method },
        headers,
        body,
        timeout: word(c, a + A_TIMEOUT) / 1000,
        mime: if mp == 0 { String::new() } else { rstr(c, mp) },
        user: if up2 == 0 { String::new() } else { rstr(c, up2) },
        pass: if pp == 0 { String::new() } else { rstr(c, pp) },
    })
}

fn run(req: &Req, cookie: Option<&str>) -> std::io::Result<(u16, String, String, Vec<u8>)> {
    let dir = std::env::temp_dir();
    let tag = format!(
        "alc-fetch-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
    );
    let hdr_path = dir.join(format!("{tag}.h"));
    let out_path = dir.join(format!("{tag}.b"));

    let mut cmd = std::process::Command::new("curl");
    cmd.args(["-sS", "-L", "--max-redirs", "5", "-D"])
        .arg(&hdr_path)
        .arg("-o")
        .arg(&out_path)
        .args(["--max-time", &req.timeout.clamp(5, 120).to_string()])
        .arg("-X")
        .arg(&req.method)
        .stdin(std::process::Stdio::piped());
    for (k, v) in &req.headers {
        cmd.arg("-H").arg(format!("{k}: {v}"));
    }
    if let Some(ck) = cookie {
        if wants_cookie(&req.url) {
            cmd.arg("-H").arg(format!("Cookie: .ROBLOSECURITY={ck}"));
        }
    }
    if !req.mime.is_empty() {
        cmd.arg("-H").arg(format!("Accept: {mime}", mime = req.mime));
    }
    if !req.user.is_empty() {
        let p = if req.pass.is_empty() {
            req.user.clone()
        } else {
            format!("{}:{}", req.user, req.pass)
        };
        cmd.arg("-u").arg(p);
    }
    if !req.body.is_empty() {
        cmd.arg("--data-binary").arg("@-");
    }

    let mut child = cmd.spawn()?;
    if !req.body.is_empty() {
        if let Some(mut si) = child.stdin.take() {
            si.write_all(&req.body)?;
        }
    }
    let st = child.wait()?;

    let raw = std::fs::read(&hdr_path).unwrap_or_default();
    let _ = std::fs::remove_file(&hdr_path);
    let body = std::fs::read(&out_path).unwrap_or_default();
    let _ = std::fs::remove_file(&out_path);
    if !st.success() {
        return Err(std::io::Error::other(format!("curl {st}")));
    }

    // last header block wins after redirects
    let text = String::from_utf8_lossy(&raw).into_owned();
    let block = text
        .split("\r\n\r\n")
        .filter(|b| b.starts_with("HTTP/"))
        .last()
        .unwrap_or("")
        .to_string();
    let mut lines = block.lines();
    let status_line = lines.next().unwrap_or("");
    let status: u16 = status_line.split_whitespace().nth(1).and_then(|s| s.parse().ok()).unwrap_or(0);
    let reason = status_line.splitn(3, ' ').nth(2).unwrap_or("").to_string();
    Ok((status, reason, block, body))
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap("env", "emscripten_start_fetch",
        |mut c: Caller<'_, St>, fetch: i32| {
            let f = fetch as u32;
            if f == 0 {
                return;
            }
            let a = f + F_ATTR;
            let onload = word(&c, a + A_ONLOAD);
            let onerror = word(&c, a + A_ONERROR);
            let onstate = word(&c, a + A_ONSTATE);
            let Some(req) = parse(&c, f) else {
                put_u16(&mut c, f + F_STATE, 4);
                put_u16(&mut c, f + F_STATUS, 0);
                let _ = callback(&mut c, onerror, f);
                return;
            };
            crate::diag::event(format!("http {} {}", req.method, req.url));
            eprintln!("[http] {} {}", req.method, req.url);
            let cookie = c.data().cookie.clone();
            let res = run(&req, cookie.as_deref());
            match res {
                Ok((status, reason, block, body)) => {
                    let n = body.len();
                    let data = if n == 0 {
                        0
                    } else {
                        let Some(inst) = c.data().inst else { return };
                        let malloc = match inst.get_typed_func::<(i32,), i32>(&mut c, "malloc") {
                            Ok(m) => m,
                            Err(_) => return,
                        };
                        match malloc.call(&mut c, (n as i32,)) {
                            Ok(p) => p as u32,
                            Err(_) => return,
                        }
                    };
                    if n > 0 {
                        wbytes(&mut c, data, &body);
                    }
                    put_u32(&mut c, f + F_DATA, data);
                    put_u64(&mut c, f + F_NUM, n as u64);
                    put_u64(&mut c, f + F_OFF, 0);
                    put_u64(&mut c, f + F_TOTAL, n as u64);
                    put_u16(&mut c, f + F_STATE, 4);
                    put_u16(&mut c, f + F_STATUS, status);
                    wbytes(&mut c, f + F_TEXT, reason.as_bytes());
                    c.data_mut().fetch_hdrs.insert(f, block.into_bytes());
                    let _ = callback(&mut c, onstate, f);
                    if status >= 200 && status < 300 {
                        let _ = callback(&mut c, onload, f);
                    } else {
                        let _ = callback(&mut c, onerror, f);
                    }
                }
                Err(e) => {
                    eprintln!("[http] {} {} failed: {e}", req.method, req.url);
                    put_u16(&mut c, f + F_STATE, 4);
                    put_u16(&mut c, f + F_STATUS, 0);
                    let _ = callback(&mut c, onstate, f);
                    let _ = callback(&mut c, onerror, f);
                }
            }
        })?;

    l.func_wrap("env", "_emscripten_fetch_get_response_headers_length",
        |c: Caller<'_, St>, fetch: i32| -> i32 {
            c.data()
                .fetch_hdrs
                .get(&(fetch as u32))
                .map(|b| b.len() as i32 + 1)
                .unwrap_or(1)
        })?;

    l.func_wrap("env", "_emscripten_fetch_get_response_headers",
        |mut c: Caller<'_, St>, fetch: i32, dst: i32, cap: i32| -> i32 {
            let Some(b) = c.data().fetch_hdrs.get(&(fetch as u32)).cloned() else {
                wbytes(&mut c, dst as u32, b"\0");
                return 1;
            };
            let n = b.len().min(cap.max(1) as usize - 1);
            wbytes(&mut c, dst as u32, &b[..n]);
            wbytes(&mut c, dst as u32 + n as u32, b"\0");
            n as i32 + 1
        })?;

    l.func_wrap("env", "emscripten_fetch_free", |mut c: Caller<'_, St>, fetch: i32| {
        c.data_mut().fetch_hdrs.remove(&(fetch as u32));
    })?;
    Ok(())
}

fn put_u32(c: &mut Caller<'_, St>, p: u32, v: u32) {
    wbytes(c, p, &v.to_le_bytes());
}

pub fn _unused(_: HashMap<u32, u32>) {}
