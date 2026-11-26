//! WebSockets, for real: tungstenite on a worker thread per socket, events
//! delivered through the engine's callbacks at safe points.
use std::collections::HashMap;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use tungstenite::Message;
use wasmtime::{Caller, Linker};

use crate::state::St;

const OPEN: u32 = 0;
const ERROR: u32 = 1;
const CLOSE: u32 = 2;
const MESSAGE: u32 = 3;

struct Sock {
    out: std::sync::mpsc::Sender<Message>,
    cb: HashMap<u32, (u32, u32)>, // event id -> (func, user)
    tid: u32,
}

static SOCKS: Mutex<Option<HashMap<u32, Sock>>> = Mutex::new(None);
static NEXT: AtomicU32 = AtomicU32::new(1);

#[derive(Clone)]
enum Evt {
    Open(u32),
    Error(u32, String),
    Close(u32, u16, String),
    Msg(u32, bool, Vec<u8>),
}

static Q: Mutex<Vec<Evt>> = Mutex::new(Vec::new());

fn push(e: Evt) {
    if let Ok(mut q) = Q.lock() {
        if q.len() < 4096 {
            q.push(e)
        }
    }
}

type Ev = Evt;

struct Table {
    id: u32,
    func: u32,
    user: u32,
}

#[derive(Clone)]
enum Ev2 {
    E(Evt),
}
type Ev3 = Ev2;
type Ev4 = Ev3;
type Ev5 = Ev4;
type Ev6 = Ev5;

pub fn register(l: &mut Linker<St>) -> wasmtime::Result<()> {
    l.func_wrap("env", "emscripten_websocket_new",
        |mut c: Caller<'_, St>, attrs: i32| -> i32 {
            let d = c.data().mem.data();
            let rd = |p: u32| unsafe { *(d[p as usize].get() as *const u32) };
            let url = crate::state::rstr(&c, rd(attrs as u32) as u32);
            let richtext = rd(attrs as u32 + 4) != 0;
            if !richtext {
                eprintln!("[ws  ] binary socket for {url}");
            }
            let (tx, rx) = std::sync::mpsc::channel::<Message>();
            let id = NEXT.fetch_add(1, Ordering::Relaxed);
            let url2 = url.clone();
            let _tid = {
                let id = id;
                let t = std::thread::Builder::new().name(format!("ws-{id}")).spawn(move || {
                    let s = match tungstenite::connect(&url2) {
                        Ok((s, _r)) => s,
                        Err(e) => {
                            push(Evt::Error(id, e.to_string()));
                            return;
                        }
                    };
                    let mut sock = s;
                    push(Evt::Open(id));
                    let mut sock2 = sock;
                    if let tungstenite::stream::MaybeTlsStream::Plain(t) = sock2.get_mut() {
                        let _ = t.set_read_timeout(Some(std::time::Duration::from_millis(30)));
                    }
                    loop {
                        match sock2.read() {
                            Ok(Message::Text(t)) => push(Evt::Msg(id, true, t.as_bytes().to_vec())),
                            Ok(Message::Binary(b)) => push(Evt::Msg(id, false, b.to_vec())),
                            Ok(Message::Close(c)) => {
                                let (code, why) = c.map(|c| (u16::from(c.code), c.reason.to_string())).unwrap_or((1000, String::new()));
                                push(Evt::Close(id, code, why));
                                break;
                            }
                            Ok(_) => {}
                            Err(tungstenite::Error::Io(e)) if e.kind() == std::io::ErrorKind::WouldBlock || e.kind() == std::io::ErrorKind::TimedOut => {}
                            Err(e) => { push(Evt::Close(id, 1006, e.to_string())); break }
                        }
                        while let Ok(m) = rx.try_recv() {
                            let close = matches!(m, Message::Close(_));
                            if sock2.send(m).is_err() { break }
                            if close { break }
                        }
                    }
                });
                if let Ok(t) = t {
                    let _ = t;
                }
                id
            };
            if let Some(map) = SOCKS.lock().unwrap().as_mut() {
                map.insert(id, Sock { out: tx, cb: HashMap::new(), tid: id });
            } else {
                let mut m = HashMap::new();
                m.insert(id, Sock { out: tx, cb: HashMap::new(), tid: id });
                *SOCKS.lock().unwrap() = Some(m);
            }
            // a reader loop is folded into the writer thread above; the socket
            // is fed by callbacks in `pump` when the engine sends
            id as i32
        })?;
    l.func_wrap("env", "emscripten_websocket_delete", |_c: Caller<'_, St>, id: i32| -> i32 {
        if let Some(map) = SOCKS.lock().unwrap().as_mut() {
            map.remove(&(id as u32));
        }
        0
    })?;
    l.func_wrap("env", "emscripten_websocket_send_binary",
        |mut c: Caller<'_, St>, id: i32, p: i32, n: i32| -> i32 {
            let d = c.data().mem.data();
            let bytes = unsafe { std::slice::from_raw_parts(d[p as usize].get(), n.max(0) as usize) }.to_vec();
            send(id as u32, Message::Binary(bytes.into()));
            0
        })?;
    l.func_wrap("env", "emscripten_websocket_send_utf8_text",
        |mut c: Caller<'_, St>, id: i32, p: i32| -> i32 {
            let s = crate::state::rstr(&c, p as u32);
            send(id as u32, Message::Text(s.into()));
            0
        })?;
    l.func_wrap("env", "emscripten_websocket_close", |_c: Caller<'_, St>, id: i32, code: i32, reason: i32| -> i32 {
        let _ = (code, reason);
        send(id as u32, Message::Close(None));
        0
    })?;
    macro_rules! on {
        ($name:literal, $ev:expr) => {
            l.func_wrap("env", $name, |id: i32, func: i32, user: i32, _t: i32| -> i32 {
                if let Some(map) = SOCKS.lock().unwrap().as_mut() {
                    if let Some(s) = map.get_mut(&(id as u32)) {
                        s.cb.insert($ev, (func as u32, user as u32));
                    }
                }
                0
            })?;
        };
    }
    on!("emscripten_websocket_set_onopen_callback_on_thread", OPEN);
    on!("emscripten_websocket_set_onerror_callback_on_thread", ERROR);
    on!("emscripten_websocket_set_onclose_callback_on_thread", CLOSE);
    on!("emscripten_websocket_set_onmessage_callback_on_thread", MESSAGE);
    Ok(())
}

fn send(id: u32, m: Message) {
    if let Some(map) = SOCKS.lock().unwrap().as_ref() {
        if let Some(s) = map.get(&id) {
            let _ = s.out.send(m);
        }
    }
}

pub fn pending() -> bool {
    Q.lock().map(|q| !q.is_empty()).unwrap_or(false)
}

pub fn pump(c: &mut wasmtime::Store<St>) {
    let evs: Vec<Evt> = { let mut q = Q.lock().unwrap(); std::mem::take(&mut *q) };
    for e in evs {
        let (id, kind) = match &e {
            Evt::Open(id) => (*id, OPEN),
            Evt::Error(id, _) => (*id, ERROR),
            Evt::Close(id, ..) => (*id, CLOSE),
            Evt::Msg(id, ..) => (*id, MESSAGE),
        };
        let Some((func, user)) = SOCKS.lock().unwrap().as_ref()
            .and_then(|m| m.get(&id)).and_then(|s| s.cb.get(&kind).copied()) else { continue };
        let inst = match c.data().inst { Some(i) => i, None => return };
        let Some(f) = inst.get_table(&mut *c, "__indirect_function_table")
            .and_then(|t| match t.get(&mut *c, func as u64) { Some(wasmtime::Ref::Func(Some(f))) => Some(f), _ => None }) else { continue };
        let base = 0x3200_0000u32 + (id * 0x2000);
        {
            let d = c.data().mem.data();
            unsafe { std::ptr::write_bytes(d[base as usize].get(), 0, 0x2000) };
            unsafe { *(d[base as usize].get() as *mut u32) = id };
        }
        match &e {
            Evt::Open(_) => {}
            Evt::Error(_, msg) => {
                let n = msg.len().min(512);
                let d = c.data().mem.data();
                for (i, b) in msg.as_bytes().iter().take(n).enumerate() {
                    unsafe { *d[base as usize + 4 + i].get() = *b }
                }
            }
            Evt::Close(_, code, reason) => {
                let d = c.data().mem.data();
                unsafe { *(d[base as usize + 4].get() as *mut u16) = *code };
                for (i, b) in reason.as_bytes().iter().take(120).enumerate() {
                    unsafe { *d[base as usize + 8 + i].get() = *b }
                }
            }
            Evt::Msg(_, text, data) => {
                let d = c.data().mem.data();
                let at = base as usize + 64;
                for (i, b) in data.iter().enumerate() {
                    unsafe { *d[at + i].get() = *b }
                }
                unsafe { *(d[base as usize + 4].get() as *mut u32) = (base + 64) };
                unsafe { *(d[base as usize + 8].get() as *mut u32) = data.len() as u32 };
                unsafe { *(d[base as usize + 12].get() as *mut u32) = *text as u32 };
            }
        }
        let mut out = [wasmtime::Val::I32(0)];
        let _ = crate::fibre::run_sync(f.call_async(&mut *c, &[
            wasmtime::Val::I32(kind as i32),
            wasmtime::Val::I32(base as i32),
            wasmtime::Val::I32(user as i32),
        ], &mut out));
    }
}

// unused aliases kept for the reader loop that lives in the writer thread
#[allow(dead_code)]
type _T = (Table, Ev6);
