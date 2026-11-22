//! Fibres on wasmtime's async machinery.
//!
//! The glue's protocol is the same one `WebAssembly.Suspending` speaks:
//!
//! * `create(tok, base, limit, entry, arg)` saves the caller under `tok`,
//!   switches the guest stack to `base`, and starts `entry` as a nested call.
//!   The caller resumes inside `create`, receiving whoever transferred to it.
//! * `transfer(from, to)` saves the caller under `from`, restores `to`'s guest
//!   stack and resumes it with `from`; the caller resumes later with whoever
//!   transfers to it.
//! * `destroy(tok)` resumes a parked context with 0.
//!
//! Every suspending import is a `func_wrap_async`; the call it is inside of
//! keeps its own wasmtime fiber, so park/resume is wasmtime's job instead of a
//! hand-rolled stack switch.  This module only holds the token table and the
//! executor that polls the resulting tasks.
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, VecDeque};
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::task::{Context, Poll, Wake, Waker};
use std::time::{Duration, Instant};

use futures::channel::oneshot;
use wasmtime::{Caller, Func, Linker, Result, Store, TypedFunc};

use crate::state::St;

const FIRST_ID: u32 = 0x9000_0000;

pub type Task = Pin<Box<dyn Future<Output = ()> + 'static>>;

/// Live (thread, fibre, guest sp) for the stall dump.
pub static TRACE_T: std::sync::Mutex<Vec<(String, u32, u32, bool)>> = std::sync::Mutex::new(Vec::new());
static PROGRESS: AtomicU64 = AtomicU64::new(0);
static TICKS: AtomicU64 = AtomicU64::new(0);
static NEXT_TASK: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0x6000_0000);

fn note(cur: u32, sp: u32) {
    if std::env::var_os("WC_HB").is_none() {
        return;
    }
    let name = std::thread::current().name().unwrap_or("?").to_string();
    let mut t = TRACE_T.lock().unwrap();
    if let Some(e) = t.iter_mut().find(|(n, _, _, _)| *n == name) {
        e.1 = cur;
        e.2 = sp;
    } else {
        t.push((name, cur, sp, true));
    }
}

thread_local! {
    static STORE: Cell<*mut Store<St>> = const { Cell::new(std::ptr::null_mut()) };
    static CUR: Cell<u32> = const { Cell::new(u32::MAX) };
    static TASKS: RefCell<HashMap<u32, Task>> = RefCell::new(HashMap::new());
    static READY: RefCell<VecDeque<u32>> = const { RefCell::new(VecDeque::new()) };
    static TIMERS: RefCell<Vec<(Instant, Waker)>> = const { RefCell::new(Vec::new()) };
}

/// Raw store handle; only ever touched from the owning thread.
#[derive(Clone, Copy)]
pub struct PStore(pub *mut Store<St>);

unsafe impl Send for PStore {}

impl PStore {
    pub fn get(&self) -> &'static mut Store<St> {
        unsafe { &mut *self.0 }
    }
}

struct Slot {
    tx: oneshot::Sender<u32>,
    sp: u32,
    base: u32,
    end: u32,
}

/// The module's own stack accessors, cached once the instance exists.
struct Ops {
    get_sp: TypedFunc<(), u32>,
    get_base: TypedFunc<(), u32>,
    get_end: TypedFunc<(), u32>,
    set_sp: TypedFunc<(u32,), ()>,
    set_limits: TypedFunc<(u32, u32), ()>,
    entry: Func,
    free: TypedFunc<(u32,), ()>,
}

pub struct Rt {
    pub store: PStore,
    /// caller token -> its parked continuation
    slots: HashMap<u32, Slot>,
    root: u32,
    next_id: u32,
    ops: Option<Ops>,
}

unsafe impl Send for Rt {}

impl Rt {
    pub fn new(store: PStore) -> Self {
        STORE.with(|c| c.set(store.0));
        Rt { store, slots: HashMap::new(), root: 0, next_id: FIRST_ID, ops: None }
    }
}

fn new_id(rt: &mut Rt) -> u32 {
    let id = rt.next_id;
    rt.next_id = id.wrapping_add(1);
    if rt.next_id < FIRST_ID {
        rt.next_id = FIRST_ID;
    }
    id
}

fn ops(st: &mut Store<St>) -> Result<Ops> {
    if let Some(o) = st.data().rt.as_ref().and_then(|r| r.ops.as_ref()) {
        return Ok(Ops {
            get_sp: o.get_sp.clone(),
            get_base: o.get_base.clone(),
            get_end: o.get_end.clone(),
            set_sp: o.set_sp.clone(),
            set_limits: o.set_limits.clone(),
            entry: o.entry.clone(),
            free: o.free.clone(),
        });
    }
    let Some(inst) = st.data().inst else {
        return Err(wasmtime::Error::msg("no instance"));
    };
    let o = Ops {
        get_sp: inst.get_typed_func(&mut *st, "get_stack_ptr")?,
        get_base: inst.get_typed_func(&mut *st, "get_stack_base")?,
        get_end: inst.get_typed_func(&mut *st, "get_stack_end")?,
        set_sp: inst.get_typed_func(&mut *st, "set_stack_ptr")?,
        set_limits: inst.get_typed_func(&mut *st, "set_stack_limits")?,
        entry: inst.get_func(&mut *st, "fiber_entry").ok_or_else(|| wasmtime::Error::msg("no fiber_entry"))?,
        free: inst.get_typed_func(&mut *st, "free")?,
    };
    let rt = st.data_mut().rt.as_mut().expect("rt");
    rt.ops = Some(Ops {
        get_sp: o.get_sp.clone(),
        get_base: o.get_base.clone(),
        get_end: o.get_end.clone(),
        set_sp: o.set_sp.clone(),
        set_limits: o.set_limits.clone(),
        entry: o.entry.clone(),
        free: o.free.clone(),
    });
    Ok(o)
}

// --- executor ------------------------------------------------------------

struct TW(u32);

impl Wake for TW {
    fn wake(self: Arc<Self>) {
        READY.with(|q| q.borrow_mut().push_back(self.0));
    }
    fn wake_by_ref(self: &Arc<Self>) {
        READY.with(|q| q.borrow_mut().push_back(self.0));
    }
}

/// Sleep until `at`.  Woken by `tick`, never by a thread park.
pub struct Sleep {
    at: Instant,
    armed: bool,
}

impl Future for Sleep {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if Instant::now() >= self.at {
            return Poll::Ready(());
        }
        if !self.armed {
            self.armed = true;
            TIMERS.with(|t| t.borrow_mut().push((self.at, cx.waker().clone())));
        }
        Poll::Pending
    }
}

/// Suspend the caller for `dur`.
pub async fn sleep(_cx: &mut Store<St>, dur: Duration) {
    Sleep { at: Instant::now() + dur, armed: false }.await
}

fn wake_due() {
    let now = Instant::now();
    TIMERS.with(|t| {
        let mut t = t.borrow_mut();
        let mut i = 0;
        while i < t.len() {
            if t[i].0 <= now {
                let (_, w) = t.remove(i);
                w.wake();
            } else {
                i += 1;
            }
        }
    });
}

fn poll_task(id: u32) -> bool {
    let waker = Waker::from(Arc::new(TW(id)));
    let Some(mut t) = TASKS.with(|m| m.borrow_mut().remove(&id)) else { return true };
    let mut cx = Context::from_waker(&waker);
    let cur = CUR.with(|c| c.get());
    CUR.with(|c| c.set(id));
    let out = t.as_mut().poll(&mut cx);
    CUR.with(|c| c.set(cur));
    PROGRESS.fetch_add(1, Ordering::Relaxed);
    TICKS.fetch_add(1, Ordering::Relaxed);
    match out {
        Poll::Ready(()) => true,
        Poll::Pending => {
            TASKS.with(|m| m.borrow_mut().insert(id, t));
            false
        }
    }
}

fn pending(id: u32) -> bool {
    TASKS.with(|m| m.borrow().contains_key(&id))
}

fn spawn(t: Task) -> Result<u32> {
    let id = NEXT_TASK.fetch_add(1, Ordering::Relaxed);
    TASKS.with(|m| m.borrow_mut().insert(id, t));
    READY.with(|q| q.borrow_mut().push_back(id));
    Ok(id)
}

/// Run queued tasks until `done` finishes (or forever if `None`).
fn drive(done: Option<u32>) {
    loop {
        let mut ran = false;
        loop {
            let Some(id) = READY.with(|q| q.borrow_mut().pop_front()) else { break };
            ran = true;
            poll_task(id);
            if done == Some(id) && !pending(id) {
                return;
            }
        }
        if let Some(d) = done {
            if !pending(d) {
                return;
            }
        }
        if !ran {
            if std::env::var_os("WC_STALL").is_some() {
                static LAST: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
                let n = TICKS.load(Ordering::Relaxed);
                let l = LAST.swap(n, Ordering::Relaxed);
                if n == l {
                    let p = STORE.with(|c| c.get());
                    let slots: Vec<String> = if p.is_null() { vec![] } else {
                        unsafe { &*p }.data().rt.as_ref().map(|r| r.slots.keys().map(|k| format!("{k:x}")).collect()).unwrap_or_default()
                    };
                    eprintln!("[drv ] {} stall done={done:?} tasks={} slots={slots:?}", std::thread::current().name().unwrap_or("?"), TASKS.with(|m| m.borrow().len()));
                }
            }
            let p = STORE.with(|c| c.get());
            if p.is_null() {
                return;
            }
            let st = unsafe { &mut *p };
            if !st.data().is_main && TICKS.load(Ordering::Relaxed) % 20 == 0 {
                let _ = crate::imports::mailbox::pump_mailbox(st);
            }
            std::thread::sleep(Duration::from_millis(1));
            wake_due();
            if done.is_none() && !pending(done.unwrap_or(0)) && TASKS.with(|m| m.borrow().is_empty()) {
                return;
            }
        }
    }
}

// --- fibre protocol ------------------------------------------------------

async fn restore(store: &mut Store<St>, tok: u32) -> Result<oneshot::Sender<u32>> {
    let o = ops(store)?;
    let slot = store.data_mut().rt.as_mut().expect("rt").slots.remove(&tok);
    let Some(slot) = slot else {
        if std::env::var_os("WC_FIB").is_some() {
            eprintln!("[fib ] MISS {tok:#x}");
        }
        return Ok(oneshot::channel().0);
    };
    o.set_limits.call_async(&mut *store, (slot.base, slot.end)).await?;
    o.set_sp.call_async(&mut *store, (slot.sp,)).await?;
    Ok(slot.tx)
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap_async("env", "__asyncjs__create_suspended_fiber_js",
        |c: Caller<'_, St>, (tok, base, limit, entry, arg): (u32, u32, u32, u32, u32)| {
            let p = c.data().rt.as_ref().expect("rt").store;
            let fut: Box<dyn Future<Output = Result<u32, wasmtime::Error>> + Send> = Box::new(async move {
                crate::trace::hit("fiber_create");
                if std::env::var_os("WC_FIB").is_some() {
                    eprintln!("[fib ] create tok={tok:#x} stack={base:x}..{limit:x} entry={entry}");
                }
                let st = un(Some(p))?;
                let o = ops(st)?;
                let sp = o.get_sp.call_async(&mut *st, ()).await?;
                let b = o.get_base.call_async(&mut *st, ()).await?;
                let e = o.get_end.call_async(&mut *st, ()).await?;
                let (tx, rx) = oneshot::channel::<u32>();
                st.data_mut().rt.as_mut().expect("rt").slots.insert(tok, Slot { tx, sp, base: b, end: e });
                // the child starts on the caller's own stack range
                o.set_limits.call_async(&mut *st, (base, limit)).await?;
                o.set_sp.call_async(&mut *st, (base,)).await?;
                let entry = entry;
                spawn(Box::pin(async move {
                    if let Err(e) = run_entry(p, tok, entry, arg).await {
                        if std::env::var_os("WC_FIB").is_some() {
                            eprintln!("[fib ] tok={tok:#x} entry={entry} ended: {e:#}");
                        }
                    }
                }))?;
                match rx.await {
                    Ok(v) => Ok(v),
                    Err(_) => Ok(0),
                }
            });
            fut
        })?;

    l.func_wrap_async("env", "__asyncjs__fiber_transfer_js",
        |c: Caller<'_, St>, (from, to): (u32, u32)| {
            let p = c.data().rt.as_ref().expect("rt").store;
            let fut: Box<dyn Future<Output = Result<u32, wasmtime::Error>> + Send> = Box::new(async move {
                crate::trace::hit("fiber_transfer");
                if from == to {
                    return Ok::<u32, wasmtime::Error>(from);
                }
                let st = un(Some(p))?;
                if std::env::var_os("WC_FIB").is_some() {
                    let slots: Vec<String> = st.data().rt.as_ref().map(|r| r.slots.keys().map(|k| format!("{k:x}")).collect()).unwrap_or_default();
                    eprintln!("[fib ] transfer {from:#x} -> {to:#x} slots={slots:?}");
                }
                if from != 0 {
                    let o = ops(st)?;
                    let sp = o.get_sp.call_async(&mut *st, ()).await?;
                    let b = o.get_base.call_async(&mut *st, ()).await?;
                    let e = o.get_end.call_async(&mut *st, ()).await?;
                    let (tx, rx) = oneshot::channel::<u32>();
                    st.data_mut().rt.as_mut().expect("rt").slots.insert(from, Slot { tx, sp, base: b, end: e });
                    restore(st, to).await?.send(from).ok();
                    return Ok(rx.await.unwrap_or(0));
                }
                // from == 0: the root stack is not a resumable context
                restore(st, to).await?.send(0).ok();
                Ok(0)
            });
            fut
        })?;

    l.func_wrap("env", "fiber_destroy_js", |mut c: Caller<'_, St>, tok: u32| -> Result<()> {
        if std::env::var_os("WC_FIB").is_some() {
            eprintln!("[fib ] {} destroy {tok:#x}", std::thread::current().name().unwrap_or("?"));
        }
        // resuming with 0 makes the dead context fall out of `fiber_entry`
        let Some(slot) = c.data_mut().rt.as_mut().expect("rt").slots.remove(&tok) else {
            return Ok(());
        };
        let p = c.data().rt.as_ref().expect("rt").store;
        spawn(Box::pin(async move {
            if let Err(e) = resume_destroy(p, slot).await {
                eprintln!("[fib ] destroy {tok:#x}: {e:#}");
            }
        }))?;
        Ok(())
    })?;
    Ok(())
}

async fn run_entry(p: PStore, tok: u32, entry: u32, arg: u32) -> Result<()> {
    let st = p.get();
    let f = ops(st)?.entry.clone();
    let mut out = [];
    f.call_async(&mut *st, &[
        wasmtime::Val::I32(tok as i32),
        wasmtime::Val::I32(entry as i32),
        wasmtime::Val::I32(arg as i32),
    ], &mut out).await
}

async fn resume_destroy(p: PStore, slot: Slot) -> Result<()> {
    let st = p.get();
    let o = ops(st)?;
    o.set_limits.call_async(&mut *st, (slot.base, slot.end)).await?;
    o.set_sp.call_async(&mut *st, (slot.sp,)).await?;
    slot.tx.send(0).ok();
    Ok(())
}

fn un(p: Option<PStore>) -> Result<&'static mut Store<St>> {
    p.map(|p| p.get()).ok_or_else(|| wasmtime::Error::msg("no store"))
}

// --- public driving API --------------------------------------------------

/// Run a task on its own guest stack.
pub fn spawn_on_new_stack<T: wasmtime::AsContextMut<Data = St>>(
    cx: &mut T, id: u32, size: u32, task: Task,
) -> Result<()> {
    let size = size.max(64 * 1024) as usize;
    if std::env::var_os("WC_TRC").is_some() { eprintln!("[fibx] spawn {id:x} stack"); }
    let (sp, base) = guest_stack(cx, size)?;
    if std::env::var_os("WC_TRC").is_some() { eprintln!("[fibx] spawn {id:x} stack {base:x} sp {sp:x}"); }
    let raw = cx.as_context().data().rt.as_ref().expect("rt").store;
    let wrapped: Task = Box::pin(async move {
        if let Ok(st) = un(Some(raw)) {
            if st.data().inst.is_some() {
                if let Ok(o) = ops(st) {
                    let _ = o.set_limits.call_async(&mut *st, (sp, base)).await;
                    let _ = o.set_sp.call_async(&mut *st, (sp,)).await;
                }
            }
        }
        task.await;
        if let Ok(st) = un(Some(raw)) {
            if st.data().inst.is_some() {
                if let Ok(o) = ops(st) {
                    let _ = o.free.call_async(&mut *st, (base,)).await;
                }
            }
        }
    });
    TASKS.with(|m| m.borrow_mut().insert(id, wrapped));
    READY.with(|q| q.borrow_mut().push_back(id));
    Ok(())
}

pub fn call_fibre<T: wasmtime::AsContextMut<Data = St>>(
    cx: &mut T, id: u32, size: u32, task: Task,
) -> Result<()> {
    spawn_on_new_stack(cx, id, size.max(2 << 20), task)?;
    drive(Some(id));
    Ok(())
}

pub fn is_live(store: &Store<St>, id: u32) -> bool {
    let _ = store;
    TASKS.with(|m| m.borrow().contains_key(&id))
}

pub fn store_ptr() -> *mut Store<St> {
    STORE.with(|c| c.get())
}

pub fn tick(_store: &mut Store<St>) {
    wake_due();
}

pub fn host_root(store: &mut Store<St>) {
    STORE.with(|c| c.set(store as *mut Store<St>));
    let rt = store.data_mut().rt.as_mut().expect("rt");
    rt.root = 0;
    rt.store.get().data_mut().root = 0;
    rt.store.get().data_mut().cur = u32::MAX;
    CUR.with(|c| c.set(u32::MAX));
}

pub fn run(store: &mut Store<St>, id: u32) {
    STORE.with(|c| c.set(store as *mut Store<St>));
    let rt = store.data_mut().rt.as_mut().expect("rt");
    rt.root = id;
    rt.store.get().data_mut().root = id;
}

/// Drive `f` to completion.
pub fn block_on<F: Future>(f: F) -> F::Output
where
    F::Output: 'static,
{
    let out: std::rc::Rc<RefCell<Option<F::Output>>> = std::rc::Rc::new(RefCell::new(None));
    let sink = out.clone();
    // SAFETY: we pump to completion before returning, so borrows in `f` stay
    // live for the whole call.
    let borrowed: Pin<Box<dyn Future<Output = F::Output> + '_>> = Box::pin(f);
    let fut: Pin<Box<dyn Future<Output = F::Output> + 'static>> = unsafe { std::mem::transmute(borrowed) };
    let id = spawn(Box::pin(async move { *sink.borrow_mut() = Some(fut.await) })).expect("spawn");
    drive(Some(id));
    let v = out.borrow_mut().take();
    match v {
        Some(v) => v,
        None => panic!("block_on: task died without a value"),
    }
}

pub fn run_sync<F: Future>(f: F) -> F::Output
where
    F::Output: 'static,
{
    block_on(f)
}

pub fn shutdown(store: &mut Store<St>) {
    let Some(rt) = store.data_mut().rt.as_mut() else { return };
    rt.slots.clear();
    TASKS.with(|m| m.borrow_mut().clear());
    READY.with(|q| q.borrow_mut().clear());
}

/// Carve a guest stack for a task. Before instantiation there is no guest
/// memory to carve from, so the task shares the caller's stack.
fn guest_stack<T: wasmtime::AsContextMut<Data = St>>(cx: &mut T, size: usize) -> Result<(u32, u32)> {
    let Some(inst) = cx.as_context().data().inst else {
        return Ok((0, 0));
    };
    let malloc = {
        let mut ccx = cx.as_context_mut();
        inst.get_typed_func::<(i32,), i32>(&mut ccx, "malloc")?
    };
    let mut ccx = cx.as_context_mut();
    let p = crate::fibre::block_on(async { malloc.call_async(&mut ccx, (size as i32,)).await })? as u32;
    if p == 0 {
        return Err(wasmtime::Error::msg("guest malloc failed for a task stack"));
    }
    Ok((((p as usize + size) & !15) as u32, p))
}

/// Fire-and-forget task from a sync import.
pub fn spawn_simple<F: Future<Output = ()> + 'static>(f: F) {
    let _ = spawn(Box::pin(f));
}
