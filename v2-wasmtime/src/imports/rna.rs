//! The engine's WebTransport layer (`rbx_wasm_rna_*`).
//!
//! On the page these are thin wrappers over the browser's `WebTransport`. Here
//! one QUIC/HTTP3 client does the same job, and completions are handed back to
//! the thread that opened the connection at its next safe point - the glue
//! does the same with a `MessageChannel`, because the engine is not re-entrant
//! from a foreign thread.
use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};

use anyhow::Result;
use wasmtime::{Caller, Linker};

use crate::state::St;

/// Receive ring shared with the engine: datagrams use kind 1, streams kind 0.
#[derive(Clone)]
struct Ring {
    data: u32,
    head: u32,
    tail: u32,
    slot: u32,
    count: u32,
}

enum Ev {
    Connected(u32, i32),
    ConnectError(u32),
    Accepted(u32, i32),
    Slot(u32, u8, i32, i32),
    Fin(u32, i32),
    StreamError(u32, i32, i32),
    WriteOk(u32, i32, i32),
    WriteErr(u32, i32),
    Closed(u32, u32, u32, i32),
    CloseComplete(u32),
}

/// A thread of its own that runs the engine's completions. On the page these
/// run from the worker's JS event loop, i.e. from outside whatever the wasm
/// thread is blocked in; here the same thing is a host thread with its own
/// store and a wasm thread identity.
static Q: OnceLock<std::sync::mpsc::Sender<Ev>> = OnceLock::new();
static ENGINE: Mutex<Option<(wasmtime::Engine, wasmtime::Module, wasmtime::SharedMemory)>> =
    Mutex::new(None);
static RINGS: Mutex<Option<HashMap<u32, Ring>>> = Mutex::new(None);
static OWNER: Mutex<Option<HashMap<u32, u32>>> = Mutex::new(None);
static TX: Mutex<Option<HashMap<(u32, u32), std::sync::Arc<tokio::sync::Mutex<wtransport::SendStream>>>>> =
    Mutex::new(None);
/// The endpoint owns the socket: dropping it closes every connection under it.
struct Open {
    conn: wtransport::Connection,
}
static KEEP: Mutex<Vec<wtransport::Endpoint<wtransport::endpoint::endpoint_side::Client>>> =
    Mutex::new(Vec::new());
static CONNS: Mutex<Option<HashMap<u32, Open>>> = Mutex::new(None);
static MEM: OnceLock<wasmtime::SharedMemory> = OnceLock::new();
/// Streams the engine has told us to stop reading (ring full / back-pressure).
/// Writes that arrive before the QUIC stream exists (the engine writes on the
/// same tick it opens a lane).
static PENDING: Mutex<Option<HashMap<(u32, u32), Vec<Vec<u8>>>>> = Mutex::new(None);
static PAUSED: Mutex<Option<std::collections::HashSet<(u32, i32)>>> = Mutex::new(None);
static STOPPED: Mutex<Option<std::collections::HashSet<(u32, i32)>>> = Mutex::new(None);
static WAKE: OnceLock<tokio::sync::Notify> = OnceLock::new();

fn wake() -> &'static tokio::sync::Notify {
    WAKE.get_or_init(tokio::sync::Notify::new)
}

async fn wait_readable(id: u32, sid: i32) {
    loop {
        let paused = PAUSED.lock().unwrap().as_ref().map(|s| s.contains(&(id, sid))).unwrap_or(false);
        if !paused {
            return;
        }
        wake().notified().await;
    }
}
static RT: OnceLock<tokio::runtime::Runtime> = OnceLock::new();

fn rt() -> &'static tokio::runtime::Runtime {
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("rna runtime")
    })
}

/// Everything is delivered on the main thread: the engine's handlers wait on
/// futexes, and doing that from inside another thread's wait point confuses the
/// fibre resume.
const MAIN_Q: u32 = u32::MAX;
static MAIN: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);

fn push(_owner: u32, ev: Ev) {
    if let Some(tx) = Q.get() {
        let _ = tx.send(ev);
    }
}

fn set_owner(id: u32, owner: u32) {
    OWNER.lock().unwrap().get_or_insert_with(HashMap::new).insert(id, owner);
}

/// Copy one payload into the next ring slot; returns the slot index and length.
fn ring_write(id: u32, data: &[u8]) -> Option<(u32, usize)> {
    let r = RINGS.lock().unwrap().as_ref()?.get(&id)?.clone();
    if r.count == 0 || r.slot == 0 {
        return None;
    }
    let mem = MEM.get()?;
    let d = mem.data();
    let ld = |p: u32| unsafe { *(d[p as usize].get() as *const u32) };
    let st = |p: u32, v: u32| unsafe { *(d[p as usize].get() as *mut u32) = v };
    let head = ld(r.head);
    if head.wrapping_sub(ld(r.tail)) >= r.count {
        return None; // full: drop, as the page does
    }
    let index = head % r.count;
    let n = data.len().min(r.slot as usize);
    let at = r.data + index * r.slot;
    for (i, b) in data.iter().take(n).enumerate() {
        unsafe { *(d[at as usize + i].get() as *mut u8) = *b };
    }
    st(r.head, head.wrapping_add(1));
    Some((index, n))
}

fn connect(id: u32, url: String, owner: u32, ring: Ring) {
    let (ring_head, ring_tail) = (ring.head, ring.tail);
    set_owner(id, owner);
    RINGS.lock().unwrap().get_or_insert_with(HashMap::new).insert(id, ring);
    rt().spawn(async move {
        let cfg = wtransport::ClientConfig::builder()
            .with_bind_default()
            .with_no_cert_validation()
            .build();
        let endpoint = match wtransport::Endpoint::client(cfg) {
            Ok(e) => e,
            Err(e) => {
                eprintln!("[rna ] endpoint: {e}");
                push(owner, Ev::ConnectError(id));
                return;
            }
        };
        let conn = match endpoint.connect(url.as_str()).await {
            Ok(c) => c,
            Err(e) => {
                eprintln!("[rna ] connect failed: {e}");
                push(owner, Ev::ConnectError(id));
                return;
            }
        };
        let max = conn.max_datagram_size().unwrap_or(0) as i32;
        eprintln!("[rna ] connected id={id} maxdgram={max}");
        push(owner, Ev::Connected(id, max));
        KEEP.lock().unwrap().push(endpoint);
        CONNS.lock().unwrap().get_or_insert_with(HashMap::new).insert(id, Open { conn: conn.clone() });

        let (up, own) = (conn.clone(), owner);
        rt().spawn(async move {
            let mut next = 3i32;
            while let Ok(mut r) = up.accept_uni().await {
                let sid = next;
                next += 4;

                eprintln!("[rna ] accepted id={id} sid={sid}");
                push(own, Ev::Accepted(id, sid));
                let mut buf = vec![0u8; 64 * 1024];
                loop {
                    wait_readable(id, sid).await;
                    let stopped = |id: u32, sid: i32| {
                        STOPPED.lock().unwrap().as_ref().map(|s| s.contains(&(id, sid))).unwrap_or(false)
                    };
                    if stopped(id, sid) {
                        r.stop(0u32.into());
                        break;
                    }
                    match r.read(&mut buf).await {
                        Ok(Some(n)) => {
                            eprintln!("[rna ] recv sid={sid} n={n}");
                            if let Some((slot, n)) = ring_write(id, &buf[..n]) {
                                push(own, Ev::Slot(id, 0, sid, (slot << 24) as i32 | n as i32));
                            } else {
                                PAUSED.lock().unwrap().get_or_insert_with(Default::default).insert((id, sid));
                            }
                        }
                        Ok(None) => {
                            push(own, Ev::Fin(id, sid));
                            break;
                        }
                        Err(e) => {
                            eprintln!("[rna ] stream {sid}: {e}");
                            push(own, Ev::StreamError(id, sid, 0));
                            break;
                        }
                    }
                }
            }
        });

        let (dg, own) = (conn.clone(), owner);
        rt().spawn(async move {
            while let Ok(d) = dg.receive_datagram().await {
                eprintln!("[rna ] dgram-in {}", d.payload().len());
                if let Some((slot, n)) = ring_write(id, &d.payload()) {
                    push(own, Ev::Slot(id, 1, 0, (slot << 24) as i32 | n as i32));
                }
            }
        });

        let (mon, mid, mhead, mtail) = (conn.clone(), id, ring_head, ring_tail);
        rt().spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                if let Some(mem) = MEM.get() {
                    let d = mem.data();
                    let ld = |p: u32| unsafe { *(d[p as usize].get() as *const u32) };
                    eprintln!("[rna ] ring head={} tail={}", ld(mhead), ld(mtail));
                }
                let _ = (&mon, mid);
            }
        });

        let cl = conn.clone();
        rt().spawn(async move {
            let e = cl.closed().await;
            eprintln!("[rna ] closed id={id}: {e:?}");
            CONNS.lock().unwrap().get_or_insert_with(HashMap::new).remove(&id);
            push(owner, Ev::Closed(id, 0, 0, 0));
        });
    });
}

fn self_ptr(c: &Caller<'_, St>) -> u32 {
    let d = c.data();
    (if d.is_main { d.main_ptr } else { d.mbx_ptr }).unwrap_or(0)
}

fn open(id: u32, sid: u32, owner: u32) {
    if stream(id, sid).is_some() {
        return;
    }
    PENDING.lock().unwrap().get_or_insert_with(HashMap::new).entry((id, sid)).or_default();
    let conn = CONNS.lock().unwrap().as_ref().and_then(|m| m.get(&id)).map(|o| o.conn.clone());
    let Some(conn) = conn else {
        push(owner, Ev::WriteErr(id, sid as i32));
        return;
    };
    rt().spawn(async move {
        match conn.open_uni().await {
            Ok(s) => {
                let Ok(mut s) = s.await else {
                    push(owner, Ev::WriteErr(id, sid as i32));
                    PENDING.lock().unwrap().get_or_insert_with(HashMap::new).remove(&(id, sid));
                    return;
                };
                let queued = PENDING.lock().unwrap().get_or_insert_with(HashMap::new).remove(&(id, sid));
                if let Some(q) = queued {
                    for b in q {
                        let _ = s.write_all(&b).await;
                    }
                }
                TX.lock().unwrap().get_or_insert_with(HashMap::new)
                    .insert((id, sid), std::sync::Arc::new(tokio::sync::Mutex::new(s)));
            }
            Err(_) => push(owner, Ev::WriteErr(id, sid as i32)),
        }
    });
}

fn stream(id: u32, sid: u32) -> Option<std::sync::Arc<tokio::sync::Mutex<wtransport::SendStream>>> {
    TX.lock().unwrap().as_ref()?.get(&(id, sid)).cloned()
}

/// A thread of its own runs the engine's completions. On the page they run from
/// the worker's JavaScript event loop - i.e. from outside whatever the wasm
/// thread is blocked in - and a wasm thread blocked in `memory.atomic.wait`
/// cannot run them here either. This thread owns a store over the same memory.
fn rna_tx() -> Option<&'static std::sync::mpsc::Sender<Ev>> {
    if let Some(t) = Q.get() {
        return Some(t);
    }
    let (engine, module, mem) = ENGINE.lock().unwrap().clone()?;
    let (tx, rx) = std::sync::mpsc::channel::<Ev>();
    std::thread::Builder::new().name("rbx-rna".into()).spawn(move || {
        if let Err(e) = run_thread(engine, module, mem, rx) {
            eprintln!("[rna ] thread died: {e:#}");
        }
    }).ok()?;
    Q.set(tx).ok()?;
    Q.get()
}

fn run_thread(
    engine: wasmtime::Engine,
    module: wasmtime::Module,
    mem: wasmtime::SharedMemory,
    rx: std::sync::mpsc::Receiver<Ev>,
) -> Result<()> {
    use wasmtime::{Extern, Linker, Store};
    let mut store = Box::new(Store::new(&engine, St::new(mem.clone())));
    let p = crate::fibre::PStore(&mut *store as *mut Store<St>);
    store.data_mut().rt = Some(crate::fibre::Rt::new(p));
    store.data_mut().engine = Some(engine.clone());
    store.data_mut().module = Some(module.clone());
    let mut l = Linker::new(&engine);
    crate::imports::register_all(&mut l)?;
    l.define(&mut *store, "env", "memory", Extern::SharedMemory(mem))?;
    let inst = l.instantiate(&mut *store, &module)?;
    store.data_mut().inst = Some(inst);
    // give it a real wasm thread identity so TLS and locks work
    let malloc = inst.get_typed_func::<(i32,), i32>(&mut *store, "malloc")?;
    let pt = malloc.call(&mut *store, (768,))? as u32;
    let ti = inst.get_typed_func::<(i32, i32, i32, i32, i32, i32), ()>(
        &mut *store, "_emscripten_thread_init")?;
    ti.call(&mut *store, (pt as i32, 0, 0, 1, 0, 0))?;
    let tls = inst.get_typed_func::<(), (i32,)>(&mut *store, "_emscripten_tls_init")?;
    tls.call(&mut *store, ())?;
    store.data_mut().mbx_ptr = Some(pt);
    crate::fibre::run(&mut *store, 0);
    eprintln!("[rna ] thread up pthread={pt:#x}");

    let mut n = 0u32;
    while let Ok(ev) = rx.recv() {
        n = n.wrapping_add(1);
        let stp = crate::fibre::store_ptr() as usize;
        let task: crate::fibre::Task = Box::pin(async move {
            let st = unsafe { &mut *(stp as *mut wasmtime::Store<St>) };
            let _ = crate::imports::mailbox::pump_mailbox(st).await;
            deliver(st, ev);
        });
        let _ = crate::fibre::call_fibre(&mut *store, 0x7200_0000 | n, 256 * 1024, task);
    }
    Ok(())
}

fn deliver(cx: &mut wasmtime::Store<St>, ev: Ev) {
    let d = cx.data();
    let Some(inst) = d.inst else { return };
    let (name, mut args): (&str, Vec<i32>) = match ev {
        Ev::Connected(id, max) => ("rbx_wasm_rna_on_connect_success", vec![id as i32, max]),
        Ev::ConnectError(id) => ("rbx_wasm_rna_on_connect_error", vec![id as i32]),
        Ev::Accepted(id, sid) => ("rbx_wasm_rna_on_stream_accepted", vec![id as i32, sid]),
        Ev::Slot(id, kind, sid, packed) => (
            "rbx_wasm_rna_on_receive_ring_slot",
            vec![id as i32, kind as i32, sid, packed >> 24, packed & 0xff_ffff],
        ),
        Ev::Fin(id, sid) => ("rbx_wasm_rna_on_stream_fin", vec![id as i32, sid]),
        Ev::StreamError(id, sid, code) =>
            ("rbx_wasm_rna_on_stream_error", vec![id as i32, sid, code]),
        Ev::WriteOk(id, sid, n) =>
            ("rbx_wasm_rna_on_stream_write_success", vec![id as i32, sid, n]),
        Ev::WriteErr(id, sid) => ("rbx_wasm_rna_on_stream_write_error", vec![id as i32, sid]),
        Ev::Closed(id, code, cls, has) => (
            "rbx_wasm_rna_on_connection_closed",
            vec![id as i32, code as i32, cls as i32, has],
        ),
        Ev::CloseComplete(id) => ("rbx_wasm_rna_on_close_complete", vec![id as i32]),
    };
    let Some(binding) = crate::imports::embind::binds().lock().unwrap().get(name).cloned() else {
        return;
    };
    let Some(table) = inst.get_table(&mut *cx, "__indirect_function_table") else { return };
    let Some(wasmtime::Ref::Func(Some(f))) = table.get(&mut *cx, binding.0 as u64) else { return };
    eprintln!("[rna ] -> {name} {args:?}");
    let mut vals = vec![wasmtime::Val::I32(binding.1 as i32)];
    for a in args.drain(..) {
        vals.push(wasmtime::Val::I32(a));
    }
    match f.call(&mut *cx, &vals, &mut []) {
        Ok(()) => {}
        Err(e) => eprintln!("[rna ] {name} threw: {e:#}"),
    }
}

/// Completions are delivered from their own thread now; kept for the callers
/// that used to drive them at wait points.
pub fn pump<T: wasmtime::AsContextMut<Data = St>>(_cx: &mut T) {}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap("env", "rbx_wasm_rna_is_supported_js", |_c: Caller<'_, St>| -> i32 { 1 })?;

    l.func_wrap(
        "env",
        "rbx_wasm_rna_connect_js",
        |c: Caller<'_, St>, id: i32, url: i32, _n: i32, _certs: i32, data: i32, head: i32,
         tail: i32, slot: i32, count: i32, _err: i32, _fix: i32, _dg: i32, _a: i32, _b: i32| {
            MEM.get_or_init(|| c.data().mem.clone());
            if let (Some(e), Some(m)) = (c.data().engine.clone(), c.data().module.clone()) {
                ENGINE.lock().unwrap().get_or_insert((e, m, c.data().mem.clone()));
            }
            let _ = rna_tx();
            let u = crate::state::rstr(&c, url as u32);
            let owner = self_ptr(&c);
            connect(
                id as u32,
                u,
                owner,
                Ring {
                    data: data as u32,
                    head: head as u32,
                    tail: tail as u32,
                    slot: slot as u32,
                    count: count as u32,
                },
            );
        },
    )?;

    l.func_wrap("env", "rbx_wasm_rna_open_stream_js", |c: Caller<'_, St>, id: i32, sid: i32| {
        eprintln!("[rna ] open id={id} sid={sid}");
        open(id as u32, sid as u32, self_ptr(&c));
    })?;

    l.func_wrap("env", "rbx_wasm_rna_write_stream_js",
        |c: Caller<'_, St>, id: i32, sid: i32, ptr: i32, len: i32| {
            let bytes = crate::state::rbytes(&c, ptr as u32, len as u32);
            eprintln!("[rna ] write id={id} sid={sid} len={len}");
            let owner = self_ptr(&c);
            let (id, sid) = (id as u32, sid as u32);
            let n = bytes.len() as i32;
            rt().spawn(async move {
                let Some(s) = stream(id, sid) else {
                    PENDING.lock().unwrap().get_or_insert_with(HashMap::new)
                        .entry((id, sid)).or_default().push(bytes);
                    return;
                };
                let mut g = s.lock().await;
                match g.write_all(&bytes).await {
                    Ok(()) => {
                        eprintln!("[rna ] wrote id={id} sid={sid} {n}");
                        push(owner, Ev::WriteOk(id, sid as i32, n))
                    }
                    Err(e) => {
                        eprintln!("[rna ] write failed id={id} sid={sid}: {e}");
                        push(owner, Ev::WriteErr(id, sid as i32))
                    }
                }
            });
        })?;

    l.func_wrap("env", "rbx_wasm_rna_send_datagram_js",
        |c: Caller<'_, St>, id: i32, ptr: i32, len: i32, _hi: i32, _lo: i32| {
            let bytes = crate::state::rbytes(&c, ptr as u32, len as u32);
            eprintln!("[rna ] dgram id={id} len={len}");
            let id = id as u32;
            let conn = CONNS.lock().unwrap().as_ref().and_then(|m| m.get(&id)).map(|o| o.conn.clone());
            if let Some(conn) = conn {
                let _ = conn.send_datagram(&bytes);
            }
        })?;

    l.func_wrap("env", "rbx_wasm_rna_close_js", |c: Caller<'_, St>, id: i32| {
        let id = id as u32;
        eprintln!("[rna ] close id={id}");
        if let Some(conn) = CONNS.lock().unwrap().as_ref().and_then(|m| m.get(&id)).map(|o| o.conn.clone()) {
            conn.close(0u32.into(), b"");
        }
        push(self_ptr(&c), Ev::CloseComplete(id));
    })?;

    l.func_wrap("env", "rbx_wasm_rna_send_connection_close_js",
        |_c: Caller<'_, St>, id: i32| {
            let id = id as u32;
            eprintln!("[rna ] conn-close id={id}");
            if let Some(conn) = CONNS.lock().unwrap().as_ref().and_then(|m| m.get(&id)).map(|o| o.conn.clone()) {
                conn.close(0u32.into(), b"");
            }
        })?;

    l.func_wrap("env", "rbx_wasm_rna_receive_ring_consumed_js",
        |c: Caller<'_, St>, id: i32, kind: i32, sid: i32| {
            eprintln!("[rna ] consumed id={id} kind={kind} sid={sid}");
            if let Some(s) = PAUSED.lock().unwrap().as_mut() {
                s.remove(&(id as u32, sid));
            }
            wake().notify_waiters();
            let _ = &c;
        })?;
    l.func_wrap("env", "rbx_wasm_rna_pause_stream_read_js",
        |_c: Caller<'_, St>, id: i32, sid: i32| {
            PAUSED.lock().unwrap().get_or_insert_with(Default::default).insert((id as u32, sid));
        })?;
    l.func_wrap("env", "rbx_wasm_rna_resume_stream_read_js",
        |_c: Caller<'_, St>, id: i32, sid: i32| {
            eprintln!("[rna ] resume id={id} sid={sid}");
            if let Some(s) = PAUSED.lock().unwrap().as_mut() {
                s.remove(&(id as u32, sid));
            }
            wake().notify_waiters();
        })?;

    l.func_wrap("env", "rbx_wasm_rna_shutdown_stream_write_js",
        |_c: Caller<'_, St>, id: i32, sid: i32| {
            if let Some(s) = stream(id as u32, sid as u32) {
                rt().spawn(async move {
                    let mut g = s.lock().await;
                    let _ = g.finish().await;
                });
            }
        })?;

    l.func_wrap("env", "rbx_wasm_rna_shutdown_stream_read_js",
        |_c: Caller<'_, St>, id: i32, sid: i32| {
            STOPPED.lock().unwrap().get_or_insert_with(Default::default).insert((id as u32, sid));
            wake().notify_waiters();
        })?;

    Ok(())
}
