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
/// The glue's XHR callbacks are tasks on the page's single JS thread, not
/// re-entrant calls into wasm from inside the request.  Queue them and let the
/// turn loop run them.
static PENDING: std::sync::Mutex<Option<std::collections::HashMap<u32, Vec<(u32, u32)>>>> =
    std::sync::Mutex::new(None);

type Done = std::io::Result<(u16, String, String, Vec<u8>)>;
struct Reply {
    me: u32,
    fetch: u32,
    onload: u32,
    onerror: u32,
    onstate: u32,
    res: Done,
    retry: bool,
}
/// Responses whose curl has finished; the owning thread writes them into guest
/// memory at its next safe point, like the page's async fetch.
static DONE: std::sync::Mutex<Vec<Reply>> = std::sync::Mutex::new(Vec::new());

/// Where this thread's deferred callbacks wait: its own mailbox, so they run at
/// its next host safe point and keep the calling thread's instance.
fn defer<T: wasmtime::AsContext<Data = St>>(cx: &T, idx: u32, fetch: u32) {
    if idx == 0 {
        return;
    }
    let d = cx.as_context().data();
    let me = if d.is_main { d.main_ptr } else { d.mbx_ptr }.unwrap_or(0);
    PENDING
        .lock()
        .unwrap()
        .get_or_insert_with(Default::default)
        .entry(me)
        .or_default()
        .push((idx, fetch));
}

fn deliver<T: wasmtime::AsContextMut<Data = St>>(cx: &mut T, r: Reply) {
    let f = r.fetch;
    if r.retry {
        // The engine's XHR wrapper occasionally misses the first resolution;
        // a second onload after a beat gets the continuation moving.
        if cx.as_context().data().fetch_hdrs.contains_key(&f) {
            defer(cx, r.onload, f);
        }
        return;
    }
    match r.res {
        Ok((status, reason, block, body)) => {
            if std::env::var_os("WC_HTTP").is_some() {
                eprintln!("[http] <- {f:#x} {status} {} bytes", body.len());
            }
            let n = body.len();
            let data = if n == 0 {
                0
            } else {
                let Some(inst) = cx.as_context().data().inst else { return };
                let Ok(malloc) = inst.get_typed_func::<(i32,), i32>(&mut *cx, "malloc") else { return };
                match malloc.call(&mut *cx, (n as i32,)) {
                    Ok(p) => p as u32,
                    Err(_) => return,
                }
            };
            let mem = cx.as_context().data().mem.clone();
            let d = mem.data();
            if n > 0 {
                for (i, b) in body.iter().enumerate() {
                    unsafe { *(d[data as usize + i].get() as *mut u8) = *b };
                }
            }
            let put = |o: u32, v: u64| unsafe {
                for i in 0..8 {
                    *(d[(f + o) as usize + i].get() as *mut u8) = (v >> (8 * i)) as u8;
                }
            };
            let put16 = |o: u32, v: u16| unsafe {
                *(d[(f + o) as usize].get() as *mut u16) = v;
            };
            let put32 = |o: u32, v: u32| unsafe {
                *(d[(f + o) as usize].get() as *mut u32) = v;
            };
            put32(0, f);
            put32(F_DATA, data);
            put(16, n as u64);
            put(24, 0);
            put(32, n as u64);
            put16(40, 4);
            put16(F_STATUS, status);
            let text = reason.as_bytes();
            for (i, b) in text.iter().enumerate() {
                unsafe { *(d[(f + F_TEXT) as usize + i].get() as *mut u8) = *b };
            }
            cx.as_context_mut().data_mut().fetch_hdrs.insert(f, block.into_bytes());
            defer(cx, r.onstate, f);
            if status >= 200 && status < 300 {
                defer(cx, r.onload, f);
                let url = {
                    let d = cx.as_context().data();
                    let p = unsafe { *(d.mem.data()[f as usize + F_URL as usize].get() as *const u32) };
                    if p == 0 { String::new() } else {
                        let dd = d.mem.data();
                        let mut v = Vec::new();
                        let mut i = p as usize;
                        loop {
                            let b = unsafe { *dd[i].get() };
                            if b == 0 || v.len() > 300 { break }
                            v.push(b);
                            i += 1;
                        }
                        String::from_utf8_lossy(&v).into_owned()
                    }
                };
                if std::env::var_os("WC_NORETRY").is_none()
                    && (url.contains("join-game") || std::env::var_os("WC_RETRYALL").is_some())
                {
                    let retry = Reply {
                        me: r.me,
                        fetch: f,
                        onload: r.onload,
                        onerror: r.onerror,
                        onstate: 0,
                        res: Ok((status, String::new(), String::new(), Vec::new())),
                        retry: true,
                    };
                    std::thread::spawn(move || {
                        std::thread::sleep(std::time::Duration::from_millis(300));
                        DONE.lock().unwrap().push(retry);
                    });
                }
            } else {
                defer(cx, r.onerror, f);
            }
        }
        Err(e) => {
            eprintln!("[http] fetch {f:#x} failed: {e}");
            let mem = cx.as_context().data().mem.clone();
            let d = mem.data();
            unsafe {
                *(d[(f + 40) as usize].get() as *mut u16) = 4;
                *(d[(f + F_STATUS) as usize].get() as *mut u16) = 0;
            }
            defer(cx, r.onstate, f);
            defer(cx, r.onerror, f);
        }
    }
}

pub fn pump<T: wasmtime::AsContextMut<Data = St>>(cx: &mut T) {
    let d = cx.as_context().data();
    let me = if d.is_main { d.main_ptr } else { d.mbx_ptr }.unwrap_or(0);
    loop {
        let hit = {
            let mut q = DONE.lock().unwrap();
            match q.iter().position(|r| mine(r.me, me)) {
                Some(i) => Some(q.remove(i)),
                None => None,
            }
        };
        let Some(r) = hit else { break };
        deliver(cx, r);
    }
    let pending = PENDING.lock().unwrap().get_or_insert_with(Default::default).remove(&me);
    let Some(pending) = pending else { return };
    let Some(inst) = cx.as_context().data().inst else { return };
    for (idx, fetch) in pending {
        let Some(t) = inst.get_table(&mut *cx, "__indirect_function_table") else { return };
        let Some(wasmtime::Ref::Func(Some(f))) = t.get(&mut *cx, idx as u64) else { continue };
        let mut tries = 0;
        while let Err(e) = f.call(&mut *cx, &[Val::I32(fetch as i32)], &mut []) {
            tries += 1;
            eprintln!("[http] callback {idx} threw: {e:#}");
            if tries == 2 {
                break;
            }
        }
    }
}

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

fn self_id(c: &Caller<'_, St>) -> u32 {
    let d = c.data();
    if d.is_main { d.main_ptr } else { d.mbx_ptr }.unwrap_or(0)
}

/// A reply for a thread that has not armed a mailbox yet (0) can be delivered
/// by any thread that reaches a safe point; the caller is only spun waiting.
fn mine(id: u32, me: u32) -> bool {
    id == me || id == 0
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

fn patch_join(body: &[u8]) -> Result<Vec<u8>, String> {
    if std::env::var_os("WC_JOINDUMP").is_some() {
        let _ = std::fs::write("/tmp/opencode/join-raw.json", body);
    }
    let mut v: serde_json::Value =
        serde_json::from_slice(body).map_err(|e| format!("body: {e}"))?;
    match v.get("status").and_then(|s| s.as_i64()) {
        Some(2) => {}
        other => return Err(format!("status {other:?}")),
    }
    let js = v.get_mut("joinScript").ok_or("no joinScript")?;
    let as_string = js.is_string();
    let mut obj: serde_json::Value = if as_string {
        serde_json::from_str(js.as_str().unwrap_or_default()).map_err(|e| format!("script: {e}"))?
    } else {
        js.clone()
    };
    if obj.get("gameFqdn").map(|x| !x.is_null()).unwrap_or(false) {
        return Err("already routed".into());
    }
    // Direct server return: the launcher sends no multiplexer address and no
    // token, the machine's own address is the route.  The engine only needs a
    // name that resolves to it (we do not validate the certificate).
    let udmux = obj
        .get("UdmuxEndpoints")
        .and_then(|u| u.get(0))
        .and_then(|e| e.get("Address"))
        .and_then(|a| a.as_str());
    if udmux.is_none() {
        let machine = obj
            .get("MachineAddress")
            .and_then(|m| m.as_str())
            .ok_or("no MachineAddress")?
            .to_string();
        ipv4(&machine).ok_or("bad MachineAddress")?;
        let port = obj.get("NetStackPort").and_then(|p| p.as_u64()).ok_or("no NetStackPort")?;
        if !(1..=65535).contains(&port) {
            return Err(format!("port {port}"));
        }
        // the transport eligibility check needs a multiplexer endpoint to read
        // an address and a port from, so stand one in for the missing one
        obj["UdmuxEndpoints"] = serde_json::json!([{ "Address": machine, "Port": port }]);
        obj["UdmuxPort"] = serde_json::json!(port);
        let host = format!("{}.sslip.io", machine.replace('.', "-"));
        eprintln!("[join] route {host} (direct)");
        obj["gameFqdn"] = serde_json::Value::String(host);
        *js = if as_string {
            serde_json::Value::String(obj.to_string())
        } else {
            obj
        };
        return Ok(v.to_string().into_bytes());
    }
    let mut native = ipv4(udmux.unwrap_or_default()).ok_or("bad udmux")?;
    if native[3] % 2 != 1 || native[3] == 255 {
        return Err(format!("udmux {}", native[3]));
    }
    native[3] += 1;
    let machine = ipv4(obj.get("MachineAddress").and_then(|m| m.as_str()).ok_or("no MachineAddress")?).ok_or("bad MachineAddress")?;
    let port = obj.get("NetStackPort").and_then(|p| p.as_u64()).ok_or("no NetStackPort")?;
    if !(1..=65535).contains(&port) {
        return Err(format!("port {port}"));
    }
    let hex = |a: [u8; 4]| a.iter().map(|b| format!("{b:02x}")).collect::<String>();
    let host = format!(
        "{}-{}-{:04x}.{}.qdmux.roblox.com",
        token_hex(obj.get("NetStackTokenValue").and_then(|t| t.as_str()).ok_or("no token")?)
            .ok_or("bad token")?,
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
    Ok(out.into_bytes())
}

fn wants_cookie(url: &str) -> bool {
    let h = host_of(url);
    h == "roblox.com" || h.ends_with(".roblox.com") || h.starts_with("rbxcdn.com")
}

#[derive(Clone)]
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

/// A fresh `gameJoinAttemptId`, so a retry is a new attempt to the launcher.
fn fresh_attempt(body: &[u8]) -> Vec<u8> {
    let mut b = match serde_json::from_slice::<serde_json::Value>(body) {
        Ok(v) => v,
        Err(_) => return body.to_vec(),
    };
    let mut raw = [0u8; 16];
    if let Ok(mut f) = std::fs::File::open("/dev/urandom") {
        use std::io::Read;
        let _ = f.read_exact(&mut raw);
    }
    let h = |i: usize| format!("{:02x}", raw[i]);
    let id = format!(
        "{}{}{}{}-{}{}-{}{}-{}{}-{}{}{}{}{}{}",
        h(0),h(1),h(2),h(3),h(4),h(5),h(6),h(7),h(8),h(9),h(10),h(11),h(12),h(13),h(14),h(15)
    );
    b["gameJoinAttemptId"] = serde_json::Value::String(id);
    b.to_string().into_bytes()
}

/// The launcher sometimes allocates direct-server-return servers, which carry
/// no multiplexer endpoint and no place to read a transport port from.  A new
/// attempt usually lands on a normal one, so retry before giving up.
fn is_dsr(body: &[u8]) -> bool {
    let Ok(v) = serde_json::from_slice::<serde_json::Value>(body) else { return false };
    if v.get("status").and_then(|s| s.as_i64()) != Some(2) {
        return false;
    }
    let js = v.get("joinScript");
    let js = match js {
        Some(serde_json::Value::String(s)) => serde_json::from_str::<serde_json::Value>(s).ok(),
        Some(v) => Some(v.clone()),
        None => None,
    };
    let Some(js) = js else { return false };
    if js.get("gameFqdn").map(|x| !x.is_null()).unwrap_or(false) {
        return false;
    }
    js.get("UdmuxEndpoints")
        .and_then(|u| u.get(0))
        .and_then(|e| e.get("Address"))
        .is_none()
}

fn run(req: &Req, cookie: Option<&str>) -> std::io::Result<(u16, String, String, Vec<u8>)> {
    if is_join(&req.url) {
        let mut body = req.body.clone();
        for attempt in 0..3 {
            let mut r = req.clone();
            r.body = body;
            let out = exec(&r, cookie)?;
            if out.0 != 200 || !is_dsr(&out.3) || attempt == 2 {
                if attempt > 0 {
                    eprintln!("[join] attempt {} gave {}/{}", attempt + 1, out.0, if is_dsr(&out.3) { "direct" } else { "routed" });
                }
                return Ok(out);
            }
            eprintln!("[join] attempt {} was direct-server-return, retrying", attempt + 1);
            body = fresh_attempt(&req.body);
        }
    }
    exec(req, cookie)
}

/// Content responses are immutable enough for a dev loop, and the CDN is the
/// slowest part of boot.  Keyed on the URL, only for GETs, never for joins.
fn cache_key(req: &Req) -> Option<std::path::PathBuf> {
    if std::env::var_os("WC_NOCACHE").is_some() || req.method != "GET" || is_join(&req.url) {
        return None;
    }
    if !(req.url.contains("rbxcdn.com") || req.url.contains("assetdelivery")) {
        return None;
    }
    let mut h: u64 = 0xcbf29ce484222325;
    for b in req.url.as_bytes() {
        h ^= *b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    Some(std::path::PathBuf::from(format!("/tmp/opencode/httpcache/{h:016x}")))
}

fn cache_get(path: &std::path::Path) -> Option<(u16, String, String, Vec<u8>)> {
    let raw = std::fs::read(path).ok()?;
    let a = u32::from_le_bytes(raw.get(0..4)?.try_into().ok()?) as usize;
    let b = u32::from_le_bytes(raw.get(4..8)?.try_into().ok()?) as usize;
    let status = u16::from_le_bytes(raw.get(8..10)?.try_into().ok()?);
    let head = raw.get(10..10 + a)?.to_vec();
    let body = raw.get(10 + a..10 + a + b)?.to_vec();
    Some((status, "OK".into(), String::from_utf8_lossy(&head).into_owned(), body))
}

fn cache_put(path: &std::path::Path, status: u16, head: &[u8], body: &[u8]) {
    let _ = std::fs::create_dir_all(path.parent().unwrap());
    let mut v = Vec::with_capacity(10 + head.len() + body.len());
    v.extend_from_slice(&(head.len() as u32).to_le_bytes());
    v.extend_from_slice(&(body.len() as u32).to_le_bytes());
    v.extend_from_slice(&status.to_le_bytes());
    v.extend_from_slice(head);
    v.extend_from_slice(body);
    let _ = std::fs::write(path, v);
}

fn exec(req: &Req, cookie: Option<&str>) -> std::io::Result<(u16, String, String, Vec<u8>)> {
    let ck = cache_key(req);
    if let Some(ck) = &ck {
        if let Some(hit) = cache_get(ck) {
            return Ok(hit);
        }
    }
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
        match patch_join(&body) {
            Ok(fixed) => {
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
            }
            Err(why) => eprintln!("[join] left unpatched: {why}"),
        }
    }
    if status == 200 {
        if let Some(ck) = &ck {
            cache_put(ck, status, block.as_bytes(), &body);
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
            eprintln!("[http] cbs onload={onload} onerror={onerror} onstate={onstate} url={}", crate::state::rstr(&c, word(&c, f + F_URL)));
            let Some(req) = parse(&c, f) else {
                put_u16(&mut c, f + F_STATE, 4);
                put_u16(&mut c, f + F_STATUS, 0);
                defer(&c, onerror, f);
                pump(&mut c);
                return;
            };
            crate::diag::event(format!("http {} {}", req.method, req.url));
            if std::env::var_os("WC_HTTP").is_some() {
                eprintln!("[http] {} {} on {}", req.method, req.url, std::thread::current().name().unwrap_or("?"));
                for (k, v) in &req.headers {
                    eprintln!("[http]   {k}: {v}");
                }
                if !req.body.is_empty() {
                    eprintln!("[http]   body {} bytes", req.body.len());
                }
            }
            let me = self_id(&c);
            // The page's fetch is a task: the engine keeps running while curl
            // works.  Blocking the caller (usually a busy worker) made the join
            // flow decide it had lost the connection.
            if std::env::var_os("WC_ASYNCHTTP").is_some() {
                let join = is_join(&req.url);
                std::thread::spawn(move || {
                    let res = run(&req, COOKIE.get().map(String::as_str));
                    if join && std::env::var_os("WC_NOHOLD").is_none() {
                        std::thread::sleep(std::time::Duration::from_millis(
                            std::env::var("WC_HOLD").ok().and_then(|v| v.parse().ok()).unwrap_or(400),
                        ));
                    }
                    DONE.lock().unwrap().push(Reply { me, fetch: f, onload, onerror, onstate, res, retry: false });
                });
            } else {
                // The engine's fetch wrapper expects the request to occupy the
                // worker until the answer is in; blocking here is what it does
                // with the page's XHR too.
                let res = run(&req, COOKIE.get().map(String::as_str));
                deliver(&mut c, Reply { me, fetch: f, onload, onerror, onstate, res, retry: false });
                pump(&mut c);
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
