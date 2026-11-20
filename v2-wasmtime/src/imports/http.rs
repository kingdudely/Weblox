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
const A_MIME: u32 = 80;
const A_HEADERS: u32 = 76;
const A_ONLOAD: u32 = 36;
const A_ONERROR: u32 = 40;
const A_ONPROGRESS: u32 = 44;
const A_ONSTATE: u32 = 48;
const A_BODY: u32 = 84;
const A_BODYLEN: u32 = 88;

const MAX_BODY: usize = 256 << 20;

/// One session, one store per thread: the cookie has to be process global.
pub static COOKIE: std::sync::OnceLock<String> = std::sync::OnceLock::new();

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

/// The join endpoints answer with the browser route only when the caller looks
/// like the native client; we then derive the `gameFqdn` the page would have
/// been given (see `docs/reference/research/native/transport-followup.md`).
fn is_join(url: &str) -> bool {
    host_of(url) == "gamejoin.roblox.com" && url.contains("/v1/join-")
}

fn ipv4(s: &str) -> Option<[u8; 4]> {
    let mut out = [0u8; 4];
    let mut n = 0;
    for part in s.split('.') {
        *out.get_mut(n)? = part.parse().ok()?;
        n += 1;
    }
    (n == 4).then_some(out)
}

fn token_hex(v: &str) -> Option<String> {
    if v.len() == 32 && v.chars().all(|c| c.is_ascii_hexdigit()) {
        return Some(v.to_ascii_lowercase());
    }
    let val = |c: u8| match c {
        b'A'..=b'Z' => Some(c - b'A'),
        b'a'..=b'z' => Some(c - b'a' + 26),
        b'0'..=b'9' => Some(c - b'0' + 52),
        b'+' => Some(62),
        b'/' => Some(63),
        b'=' => Some(0),
        _ => None,
    };
    let bs = v.as_bytes();
    if bs.len() % 4 != 0 {
        return None;
    }
    let pad = bs.iter().rev().take_while(|c| **c == b'=').count();
    let mut out = Vec::new();
    for chunk in bs.chunks(4) {
        let [a, b, c, d] = [val(chunk[0])?, val(chunk[1])?, val(chunk[2])?, val(chunk[3])?];
        let n = (a as u32) << 18 | (b as u32) << 12 | (c as u32) << 6 | d as u32;
        out.extend_from_slice(&[(n >> 16) as u8, (n >> 8) as u8, n as u8]);
    }
    out.truncate(out.len().saturating_sub(pad));
    Some(out.iter().map(|b| format!("{b:02x}")).collect())
}

fn patch_join(body: &[u8]) -> Option<Vec<u8>> {
    if std::env::var_os("WC_JOINDUMP").is_some() {
        let _ = std::fs::write("/tmp/opencode/join-raw.json", body);
    }
    let mut v: serde_json::Value = serde_json::from_slice(body).ok()?;
    if v.get("status")?.as_i64()? != 2 {
        return None;
    }
    let js = v.get_mut("joinScript")?;
    let as_string = js.is_string();
    let mut obj: serde_json::Value = if as_string {
        serde_json::from_str(js.as_str()?).ok()?
    } else {
        js.clone()
    };
    if obj.get("gameFqdn").map(|x| !x.is_null()).unwrap_or(false) {
        return None;
    }
    let mut native = ipv4(obj.get("UdmuxEndpoints")?.get(0)?.get("Address")?.as_str()?)?;
    if native[3] % 2 != 1 || native[3] == 255 {
        return None;
    }
    native[3] += 1;
    let machine = ipv4(obj.get("MachineAddress")?.as_str()?)?;
    let port = obj.get("NetStackPort")?.as_u64()?;
    if !(1..=65535).contains(&port) {
        return None;
    }
    let hex = |a: [u8; 4]| a.iter().map(|b| format!("{b:02x}")).collect::<String>();
    let host = format!(
        "{}-{}-{:04x}.{}.qdmux.roblox.com",
        token_hex(obj.get("NetStackTokenValue")?.as_str()?)?,
        hex(machine),
        port,
        hex(native)
    );
    eprintln!("[join] route {host}");
    obj["gameFqdn"] = serde_json::Value::String(host);
    *js = if as_string {
        serde_json::Value::String(obj.to_string())
    } else {
        obj
    };
    let out = v.to_string();
    if std::env::var_os("WC_JOINDUMP").is_some() {
        let _ = std::fs::write("/tmp/opencode/join-patched.json", &out);
    }
    Some(out.into_bytes())
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
    // the method is an inline char buffer, not a pointer
    let method = rstr(c, a + A_METHOD);
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
        .arg(&req.url)
        .stdin(std::process::Stdio::piped());
    let join = is_join(&req.url);
    for (k, v) in &req.headers {
        if join && k.eq_ignore_ascii_case("rbx-user-agent") {
            continue;
        }
        cmd.arg("-H").arg(format!("{k}: {v}"));
    }
    if join {
        cmd.arg("-H").arg("rbx-user-agent: Roblox/Darwin");
    }
    if wants_cookie(&req.url) {
        // what the page's XHR always adds on a cross-origin POST
        cmd.arg("-H").arg("Origin: https://play.roblox.com");
        cmd.arg("-H").arg("Referer: https://play.roblox.com/");
        cmd.arg("-H").arg("Accept: */*");
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

    if std::env::var_os("WC_CURL").is_some() {
        eprintln!("[curl] {cmd:?}");
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
    let mut body = std::fs::read(&out_path).unwrap_or_default();
    let _ = std::fs::remove_file(&out_path);
    if !st.success() {
        return Err(std::io::Error::other(format!("curl {st}")));
    }

    // last header block wins after redirects
    let text = String::from_utf8_lossy(&raw).into_owned();
    let mut block = text
        .split("\r\n\r\n")
        .filter(|b| b.starts_with("HTTP/"))
        .last()
        .unwrap_or("")
        .to_string();
    let mut lines = block.lines();
    let status_line = lines.next().unwrap_or("");
    let status: u16 = status_line.split_whitespace().nth(1).and_then(|s| s.parse().ok()).unwrap_or(0);
    let reason = status_line.splitn(3, ' ').nth(2).unwrap_or("").to_string();
    if is_join(&req.url) {
        eprintln!("[join] {status} {}", String::from_utf8_lossy(&body[..body.len().min(400)]).replace('\n', " "));
    }
    if status == 200 && is_join(&req.url) && std::env::var_os("WC_NOPATCH").is_none() {
        if let Some(fixed) = patch_join(&body) {
            body = fixed;
            let mut out = String::new();
            for line in block.lines().skip(1) {
                if line.to_ascii_lowercase().starts_with("content-length:") {
                    out.push_str(&format!("Content-Length: {}\r\n", body.len()));
                } else {
                    out.push_str(line);
                    out.push_str("\r\n");
                }
            }
            out.push_str("\r\n");
            block = out.clone();
            if std::env::var_os("WC_JOINDUMP").is_some() {
                let _ = std::fs::write("/tmp/opencode/join-hdrs.txt", &block);
            }
        } else {
            eprintln!("[join] response left unpatched");
        }
    }
    Ok((status, reason, block, body))
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    if std::env::var_os("WC_HTTP").is_some() {
        eprintln!("[http] register");
    }
    l.func_wrap("env", "emscripten_start_fetch",
        |mut c: Caller<'_, St>, fetch: i32| {
            crate::trace::hit("http_start");
            let f = fetch as u32;
            if std::env::var_os("WC_HTTP").is_some() {
                eprintln!("[http] start fetch={f:#x}");
            }
            if f == 0 {
                return;
            }
            // the engine passes `*(fetch + 0)` (the glue's XHR handle) to the
            // response header calls, so make the fetch its own handle
            put_u32(&mut c, f, f);
            let a = f + F_ATTR;
            let onload = word(&c, a + A_ONLOAD);
            let onerror = word(&c, a + A_ONERROR);
            let onstate = word(&c, a + A_ONSTATE);
            if std::env::var_os("WC_HTTP").is_some() {
                let up = word(&c, f + F_URL);
                eprintln!("[http] start f={f:#x} url={:?} state={}", if up == 0 { String::new() } else { rstr(&c, up) }, word(&c, f + F_STATE));
            }
            let Some(req) = parse(&c, f) else {
                put_u16(&mut c, f + F_STATE, 4);
                put_u16(&mut c, f + F_STATUS, 0);
                let _ = callback(&mut c, onerror, f);
                return;
            };
            crate::diag::event(format!("http {} {}", req.method, req.url));
            eprintln!("[http] {} {} on {}", req.method, req.url, std::thread::current().name().unwrap_or("?"));
            if std::env::var_os("WC_HTTP").is_some() {
                for (k, v) in &req.headers {
                    eprintln!("[http]   {k}: {v}");
                }
                if !req.body.is_empty() {
                    eprintln!("[http]   body {} bytes", req.body.len());
                }
                let a = f + F_ATTR;
                let d = c.data().mem.data();
                let _ = &d;


            }
            let res = run(&req, COOKIE.get().map(String::as_str));
            match res {
                Ok((status, reason, block, body)) => {
                    if std::env::var_os("WC_HTTP").is_some() {
                        eprintln!("[http] <- {status} {} bytes", body.len());
                    }
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
            if std::env::var_os("WC_JOINDUMP").is_some() {
                eprintln!("[http] hdrlen {}", fetch as u32);
            }
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
        if std::env::var_os("WC_HTTP").is_some() {
            let f = fetch as u32;
            let up = word(&c, f + F_URL);
            eprintln!("[http] free {f:#x} url={:?} state={} err={}",
                      if up == 0 { String::new() } else { rstr(&c, up) },
                      word(&c, f + F_STATE), word(&c, f + 36));
        }
        c.data_mut().fetch_hdrs.remove(&(fetch as u32));
    })?;
    Ok(())
}

fn put_u32(c: &mut Caller<'_, St>, p: u32, v: u32) {
    wbytes(c, p, &v.to_le_bytes());
}

pub fn _unused(_: HashMap<u32, u32>) {}
