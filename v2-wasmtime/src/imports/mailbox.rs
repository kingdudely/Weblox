//! Thread mailbox, ported from the page glue.
//!
//! The glue's `_emscripten_thread_mailbox_await(ptr)` does not block: it arms
//! `Atomics.waitAsync(ptr, ptr)` and marks `ptr+112`. The ring itself is a
//! message from the sender, and the *receiver* then runs
//! `_emscripten_check_mailbox()` from a fresh task (`runAndCheckMailbox`).
//!
//! A blocking thread cannot run a fresh task, so here:
//!   * a worker, which has nothing else to do, parks in the await and drains
//!     when rung;
//!   * the main thread only arms and returns, and drains at its next safe
//!     point (`main_drain`, called from the turn loop and from any suspending
//!     import) exactly like the glue's `Ke`.
//!
//! Rings are counted, not edge-triggered, so a ring that arrives while the
//! target is busy is not lost.
use std::collections::HashMap;
use std::sync::Mutex;

use wasmtime::Result;
use futures::channel::oneshot;
use wasmtime::{AsContextMut, Caller, Linker};

use crate::state::St;

static RINGS: Mutex<Option<HashMap<u32, u32>>> = Mutex::new(None);
/// Bumped for every ring anywhere; a thread can test this without taking the
/// map lock (which `emscripten_get_now` cannot afford).
static RINGS_TOTAL: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
static WAITERS: Mutex<Option<HashMap<u32, oneshot::Sender<()>>>> = Mutex::new(None);

fn take_ring(target: u32) -> bool {
    let mut g = RINGS.lock().unwrap();
    let m = g.get_or_insert_with(HashMap::new);
    match m.get_mut(&target) {
        Some(n) if *n > 0 => {
            *n -= 1;
            true
        }
        _ => false,
    }
}

fn add_ring(target: u32) -> bool {
    RINGS_TOTAL.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let mut g = RINGS.lock().unwrap();
    *g.get_or_insert_with(HashMap::new).entry(target).or_insert(0) += 1;
    let mut w = WAITERS.lock().unwrap();
    w.get_or_insert_with(HashMap::new).remove(&target).is_some()
}

fn arm(target: u32) -> oneshot::Receiver<()> {
    let (tx, rx) = oneshot::channel();
    WAITERS
        .lock()
        .unwrap()
        .get_or_insert_with(HashMap::new)
        .insert(target, tx);
    rx
}

fn disarm(target: u32) {
    WAITERS.lock().unwrap().get_or_insert_with(HashMap::new).remove(&target);
}


/// The glue's `runAndCheckMailbox`: re-arm, then drain. Called at this
/// thread's safe points (the turn loop on the main thread, a wait on a
/// worker).
pub async fn pump_mailbox<T: AsContextMut<Data = St>>(cx: &mut T) -> bool {
    let d = cx.as_context().data();
    // A thread can reach a safe point before it arms a mailbox; the deferred
    // work is ours to run either way.
    let ptr = if d.is_main { d.main_ptr } else { d.mbx_ptr };
    if let Some(ptr) = ptr {
        // The sender only calls the notify import when the target is parked in
        // `Atomics.waitAsync`; a busy target just gets the ring appended.  So
        // the ring cannot be counted from here - always run the drain.
        let rang = take_ring(ptr);
        if rang && std::env::var_os("WC_MBX").is_some() {
            eprintln!("[mbx ] ring {} {ptr:#x}", std::thread::current().name().unwrap_or("?"));
        }
        set_ready(cx, ptr);
    }
    crate::imports::http::pump(cx).await;
    crate::imports::rna::pump(cx).await;
    if ptr.is_some() {
        check(cx).await;
    }
    true
}

fn pending(target: u32) -> bool {
    RINGS.lock().unwrap().as_ref().and_then(|m| m.get(&target)).copied().unwrap_or(0) > 0
}

/// A worker runs guest code for long stretches and never reaches the idle loop,
/// so its mailbox has to be drained from a point inside that code the way the
/// page's event loop would.  Called from a frequently used import; only does
/// work when something is actually queued.
pub fn pump_now(c: &mut Caller<'_, St>) {
    thread_local! {
        static SEEN: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
    }
    let total = RINGS_TOTAL.load(std::sync::atomic::Ordering::Relaxed);
    if SEEN.with(|s| s.get()) == total {
        return;
    }
    SEEN.with(|s| s.set(total));
    if c.data().is_main || c.data().in_drain {
        return;
    }
    let Some(ptr) = c.data().mbx_ptr else { return };
    if !pending(ptr) {
        return;
    }
    let _ = crate::fibre::run_sync(pump_mailbox(c));
}

/// `pump_mailbox` for the main thread only.
pub async fn drop_in<T: AsContextMut<Data = St>>(cx: &mut T) -> bool {
    if !cx.as_context().data().is_main {
        return false;
    }
    crate::imports::boot::run_main_jobs();
    if cx.as_context().data().main_ptr.is_none() {
        let inst = cx.as_context().data().inst;
        if let Some(inst) = inst {
            if let Ok(f) = inst.get_typed_func::<(), (i32,)>(&mut *cx, "pthread_self") {
                if let Ok((me,)) = f.call_async(&mut *cx, ()).await {
                    if me != 0 {
                        let me = me as u32;
                        cx.as_context_mut().data_mut().main_ptr = Some(me);
                        set_ready(cx, me);
                    }
                }
            }
        }
    }
    pump_mailbox(cx).await
}

fn set_ready<T: AsContextMut<Data = St>>(cx: &mut T, ptr: u32) {
    let mem = cx.as_context().data().mem.clone();
    unsafe { *(mem.data()[ptr as usize + 112].get() as *mut u32) = 1 }
}

/// The glue's `Ke` loop: watch our mailbox word, drain when it is rung.
fn spawn_loop(c: &mut Caller<'_, St>, ptr: u32) -> Result<()> {
    let p = c.data().rt.as_ref().expect("rt").store;
    let id = 0xFFFF_FE00 | (ptr & 0xFF);
    let task: crate::fibre::Task = Box::pin(async move {
        let st = p.get();
        let mem = st.data().mem.clone();
        loop {
            crate::fibre::sleep(&mut *st, std::time::Duration::from_millis(1)).await;
            let w = unsafe { *(mem.data()[ptr as usize].get() as *const u32) };
            if w == ptr {
                continue;
            }
            unsafe { *(mem.data()[ptr as usize + 112].get() as *mut u32) = 1 }
            check(st).await;
        }
    });
    crate::fibre::spawn_on_new_stack(p.get(), id, 96 * 1024, task)?;
    Ok(())
}

async fn check<T: AsContextMut<Data = St>>(cx: &mut T) {
    if cx.as_context().data().in_drain {
        return;
    }
    let Some(inst) = cx.as_context().data().inst else {
        return;
    };
    let Some(f) = inst
        .get_export(&mut *cx, "_emscripten_check_mailbox")
        .and_then(|e| e.into_func())
    else {
        return;
    };
    if std::env::var_os("WC_MBX").is_some() {
        let d = cx.as_context().data();
        let p = if d.is_main { d.main_ptr } else { d.mbx_ptr };
        if let Some(p) = p {
            let mem = d.mem.clone();
            let w = |o: u32| unsafe { *(mem.data()[p as usize + o as usize].get() as *const u32) };
            let mbx = w(104);
            let q = |o: u32| if mbx == 0 { 0 } else { unsafe { *(mem.data()[mbx as usize + o as usize].get() as *const u32) } };
            let (a, b) = (q(44), q(48));
            if a != b {
                eprintln!("[mbx ] check {} mbx={mbx:#x} q={a}/{b}", std::thread::current().name().unwrap_or("?"));
            }
        }
    }
    cx.as_context_mut().data_mut().in_drain = true;
    if let Err(e) = f.call_async(&mut *cx, &[], &mut []).await {
        static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if n < 5 {
            eprintln!("[mbx] check_mailbox threw: {e:#}");
        }
    }
    cx.as_context_mut().data_mut().in_drain = false;
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap("env", "_emscripten_thread_mailbox_await",
        |mut c: Caller<'_, St>, ptr: i32| {
            crate::trace::hit("mbx_await");
            let ptr = ptr as u32;

            if c.data().is_main {
                c.data_mut().main_ptr = Some(ptr);
                set_ready(&mut c, ptr);
                // the glue arms and returns; the drain happens from the loop
                if !c.data().mbx_task {
                    c.data_mut().mbx_task = true;
                    spawn_loop(&mut c, ptr).expect("mailbox loop");
                }
                return;
            }

            // as on the page: arm and return; the ring is drained at this
            // thread's next safe point (`pump_mailbox`)
            if std::env::var_os("WC_MBX").is_some() {
                eprintln!("[mbx ] arm {} {ptr:#x}", std::thread::current().name().unwrap_or("?"));
            }
            c.data_mut().mbx_ptr = Some(ptr);
            set_ready(&mut c, ptr);
        })?;

    l.func_wrap("env", "_emscripten_notify_mailbox_postmessage",
        |mut c: Caller<'_, St>, target: i32, self_thread: i32| {
            crate::trace::hit("mbx_notify");
            crate::diag::notify();
            if std::env::var_os("WC_MBX").is_some() {
                let mem = c.data().mem.clone();
                let w = |a: u32, o: u32| unsafe { *(mem.data()[a as usize + o as usize].get() as *const u32) };
                let mbx = w(target as u32, 104);
                let q = |o: u32| if mbx == 0 { 0 } else { w(mbx, o) };
                eprintln!("[mbx ] notify {target:#x} from {self_thread:#x} mbx={mbx:#x} q={}/{}", q(44), q(48));
            }
            add_ring(target as u32);
            // the receiver is parked in `Atomics.waitAsync` on its mailbox
            // word; a post has to wake it there
            if target as u32 != self_thread as u32 {
                let mem = c.data().mem.clone();
                let _ = mem.atomic_notify(target as u64, 1);
            }
        })?;
    Ok(())
}

async fn sleep_ms(ms: u64) {
    let (tx, rx) = oneshot::channel::<()>();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_millis(ms));
        let _ = tx.send(());
    });
    let _ = rx.await;
}
