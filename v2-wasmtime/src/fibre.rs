//! The guest's fibre scheduler, on real host stacks.
//!
//! The glue switches contexts with a symmetric-coroutine token protocol:
//!
//! * `create(tok, base, limit, entry, arg)` parks the caller under `tok`, moves
//!   the guest stack to the new range and runs `entry`.
//! * `transfer(from, to)` parks the caller under `from` and resumes whoever is
//!   parked under `to`; both return the token of whoever resumes them.
//! * `destroy(tok)` resumes a parked context with 0; it is expected to die on
//!   the guest's own `unreachable`.
//!
//! A context is therefore either parked somewhere or running, and resumption is
//! always a token handed over by somebody else.  Stackless futures are enough
//! for the first half, but not the second: a chain A->B->C that comes back to A
//! has to resume *through* the frames in between, so every context keeps a host
//! stack (`wasmtime-internal-fiber`) plus its guest stack pointer and limits.
//!
//! Transfers never switch directly.  They suspend the caller, which unwinds to
//! the trampoline below; the trampoline takes the target the suspend left
//! behind and keeps draining until nothing is queued.  That is emscripten's
//! `$Fibers` model exactly, with the JSPI unwind replaced by the host stack
//! switch.
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

use wasmtime::{Caller, Linker, Result, Store, TypedFunc};
use wasmtime_internal_fiber::{Fiber, FiberStack, Suspend};

use crate::state::St;

const TASK_STACK: usize = 2 << 20;
const FIBRE_STACK: usize = 512 << 10;
const FIRST_ID: u32 = 0x9000_0000;

/// What the trampoline runs next: a fibre, the value handed back to the
/// `suspend` that parked it, and the guest stack to restore first (its own
/// last one when `None`).
struct Step {
    fibre: u32,
    val: u32,
    ctx: Option<(u32, u32, u32)>,
}

type Yield = Option<Step>;
type Fib = Fiber<'static, u32, Yield, ()>;
type Susp = Suspend<u32, Yield, ()>;

pub type Task = Pin<Box<dyn Future<Output = ()> + Send + 'static>>;

/// Live (thread, fibre, guest sp) for the stall dump.
pub static TRACE_T: std::sync::Mutex<Vec<(String, u32, u32, bool)>> = std::sync::Mutex::new(Vec::new());

fn note(cur: u32, sp: u32) {
    if std::env::var_os("WC_HB").is_none() {
        return;
    }
    let parked = {
        let st = STORE.with(|c| c.get());
        if st.is_null() {
            false
        } else {
            waiting(unsafe { &*st }, cur)
        }
    };
    let name = std::thread::current().name().unwrap_or("?").to_string();
    let mut t = TRACE_T.lock().unwrap();
    if let Some(e) = t.iter_mut().find(|(n, _, _, _)| *n == name) {
        e.1 = cur;
        e.2 = sp;
        e.3 = parked;
    } else {
        t.push((name, cur, sp, parked));
    }
}

thread_local! {
    /// Fibre currently executing on this thread (`u32::MAX` off a fibre).
    static CUR: Cell<u32> = const { Cell::new(u32::MAX) };
    /// Sleeping contexts: deadline, fibre, guest stack.
    static SLEEPERS: RefCell<Vec<(std::time::Instant, u32, (u32, u32, u32))>> =
        const { RefCell::new(Vec::new()) };
    static STORE: Cell<*mut Store<St>> = const { Cell::new(std::ptr::null_mut()) };
    /// Work the trampoline still has to run.
    static NEXT: RefCell<Option<Step>> = const { RefCell::new(None) };
    /// Whether the trampoline loop is already on this thread's stack.
    static TRAMP: Cell<bool> = const { Cell::new(false) };
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

struct F {
    susp: Rc<Cell<*mut Susp>>,
    f: Box<Fib>,
    /// Guest stack of this fibre as it was when it last parked.
    ctx: Cell<(u32, u32, u32)>,
    /// malloc'd guest stack to release when the fibre finishes, if any.
    guest: (u32, u32),
}

/// The module's own stack accessors, cached once the instance exists.
struct Ops {
    get_sp: TypedFunc<(), u32>,
    get_base: TypedFunc<(), u32>,
    get_end: TypedFunc<(), u32>,
    set_sp: TypedFunc<(u32,), ()>,
    set_limits: TypedFunc<(u32, u32), ()>,
    entry: TypedFunc<(u32, u32, u32), ()>,
    free: TypedFunc<(u32,), ()>,
}

pub struct Rt {
    pub store: PStore,
    /// token -> context parked under it
    slots: HashMap<u32, u32>,
    /// host fibre -> its context
    fibres: HashMap<u32, F>,
    root: u32,
    next_id: u32,
    ops: Option<Ops>,
}

unsafe impl Send for Rt {}

impl Rt {
    pub fn new(store: PStore) -> Self {
        STORE.with(|c| c.set(store.0));
        Rt { store, slots: HashMap::new(), fibres: HashMap::new(), root: 0, next_id: FIRST_ID, ops: None }
    }
}

enum Resume {
    Done,
    Parked(Yield),
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
        entry: inst.get_typed_func(&mut *st, "fiber_entry")?,
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

/// Guest stack of the running context.
fn here(st: &mut Store<St>) -> Result<(u32, u32, u32)> {
    let o = ops(st)?;
    Ok((o.get_sp.call(&mut *st, ())?, o.get_base.call(&mut *st, ())?, o.get_end.call(&mut *st, ())?))
}

/// Park this fibre and hand the trampoline its next step.
fn susp_here(step: Yield) -> u32 {
    let cur = CUR.with(|c| c.get());
    if std::env::var_os("WC_FX").is_some() {
        let n = step.as_ref().map(|s| s.fibre).unwrap_or(0);
        eprintln!("[fbr ] park cur={cur:#x} -> {n:#x}");
    }
    let st = STORE.with(|c| c.get());
    assert!(!st.is_null() && cur != u32::MAX, "suspend outside a fibre");
    let p = unsafe { &*st }
        .data()
        .rt
        .as_ref()
        .and_then(|r| r.fibres.get(&cur))
        .map(|f| f.susp.get())
        .filter(|p| !p.is_null())
        .expect("suspend outside a fibre");
    unsafe { (*p).suspend(step) }
}

/// Run `step` to its next suspension.
fn resume(st: &mut Store<St>, step: Step) -> Result<Resume> {
    let p = match st.data().rt.as_ref().and_then(|r| r.fibres.get(&step.fibre)) {
        Some(f) => f.susp.get(),
        None => return Ok(Resume::Done),
    };
    let ctx = match step.ctx {
        Some(c) => c,
        None => st.data().rt.as_ref().expect("rt").fibres.get(&step.fibre).expect("fibre").ctx.get(),
    };

    // Before instantiation there is no module state to move.
    let o = if st.data().inst.is_some() {
        let o = ops(st)?;
        o.set_limits.call(&mut *st, (ctx.1, ctx.2))?;
        o.set_sp.call(&mut *st, (ctx.0,))?;
        Some(o)
    } else {
        None
    };

    if std::env::var_os("WC_FX").is_some() {
        eprintln!("[fbr ] run {:#x} val={:#x} ctx={ctx:?}", step.fibre, step.val);
    }
    note(step.fibre, ctx.0);
    let prev = CUR.with(|c| c.get());
    CUR.with(|c| c.set(step.fibre));
    st.data_mut().cur = step.fibre;
    let out = {
        let rt = st.data_mut().rt.as_mut().expect("rt");
        rt.fibres.get_mut(&step.fibre).expect("fibre").f.resume(step.val)
    };
    CUR.with(|c| c.set(prev));
    st.data_mut().cur = prev;

    Ok(match out {
        Ok(()) => {
            if let Some(f) = st.data_mut().rt.as_mut().expect("rt").fibres.remove(&step.fibre) {
                if let (Some(o), true) = (&o, f.guest.0 != 0) {
                    let _ = o.free.call(&mut *st, (f.guest.0,));
                }
            }
            Resume::Done
        }
        Err(next) => {
            // Remember where the guest parked, for the next resumer.
            if st.data().inst.is_some() {
                if let Ok(ctx) = here(st) {
                    if let Some(f) = st.data().rt.as_ref().and_then(|r| r.fibres.get(&step.fibre)) {
                        f.ctx.set(ctx);
                    }
                }
            }
            Resume::Parked(next)
        }
    })
}

/// Drain pending contexts until nothing is queued.
/// Hand a context to the trampoline.  Re-entrant calls only leave the step in
/// `NEXT`: the single loop that is already running picks it up, so nested
/// fibres never deepen any host stack.
fn schedule(st: &mut Store<St>, step: Step) {
    NEXT.with(|n| *n.borrow_mut() = Some(step));
    trampoline(st);
}

fn trampoline(st: &mut Store<St>) {
    if TRAMP.with(|t| t.replace(true)) {
        return;
    }
    loop {
        while let Some(s) = NEXT.with(|n| n.borrow_mut().take()) {
            match resume(st, s) {
                Ok(Resume::Done) => {}
                Ok(Resume::Parked(n)) => {
                    if let Some(n) = n {
                        NEXT.with(|q| *q.borrow_mut() = Some(n));
                    }
                }
                Err(e) => eprintln!("[fbr ] scheduler: {e:#}"),
            }
        }
        // Nothing queued: let the timers have a turn before parking.
        tick(st);
        if NEXT.with(|n| n.borrow().is_none()) {
            break;
        }
    }
    TRAMP.with(|t| t.set(false));
}

fn spawn(st: &mut Store<St>, id: u32, size: usize, tok: u32, entry: u32, arg: u32) -> Result<()> {
    let raw = st.data().rt.as_ref().expect("rt").store;
    let stack = FiberStack::new(size, false).map_err(|e| wasmtime::Error::msg(e.to_string()))?;
    let slot: Rc<Cell<*mut Susp>> = Rc::new(Cell::new(std::ptr::null_mut()));
    let ss = slot.clone();
    let fib = Fiber::new(stack, move |_first: u32, s: &mut Susp| {
        ss.set(s as *mut Susp);
        let st = raw.get();
        let Ok(o) = ops(st) else { return };
        if let Err(e) = o.entry.call(st, (tok, entry, arg)) {
            if std::env::var_os("WC_FX").is_some() {
                eprintln!("[fbr ] fibre tok={tok:#x} entry={entry} died: {e:#}");
            }
        }
    })
    .map_err(|(e, _)| wasmtime::Error::msg(e.to_string()))?;
    st.data_mut().rt.as_mut().expect("rt").fibres.insert(id, F { susp: slot, f: Box::new(fib), ctx: Cell::new((0, 0, 0)), guest: (0, 0) });
    Ok(())
}

/// `__asyncjs__create_suspended_fiber_js(tok, base, limit, entry, arg)`.
pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap("env", "__asyncjs__create_suspended_fiber_js",
        |mut c: Caller<'_, St>, tok: u32, base: u32, limit: u32, entry: u32, arg: u32| -> Result<u32> {
            crate::trace::hit("fiber_create");
            let cur = c.data().cur;
            if cur == u32::MAX {
                return Err(wasmtime::Error::msg("fiber create from the host root"));
            }
            let raw = c.data().rt.as_ref().expect("rt").store;
            let id = new_id(c.data_mut().rt.as_mut().expect("rt"));
            if std::env::var_os("WC_FX").is_some() {
                eprintln!("[fbr ] create tok={tok:#x} base={base:#x} limit={limit:#x} entry={entry} n={}",
                    c.data().rt.as_ref().unwrap().fibres.len());
            }
            spawn(raw.get(), id, FIBRE_STACK, tok, entry, arg)?;
            c.data_mut().rt.as_mut().expect("rt").slots.insert(tok, cur);
            Ok(susp_here(Some(Step { fibre: id, val: 0, ctx: Some((base, base, limit)) })))
        })?;

    l.func_wrap("env", "__asyncjs__fiber_transfer_js",
        |mut c: Caller<'_, St>, from: u32, to: u32| -> Result<u32> {
            crate::trace::hit("fiber_transfer");
            let cur = c.data().cur;
            if from == to {
                return Ok(from);
            }
            if from != 0 {
                c.data_mut().rt.as_mut().expect("rt").slots.insert(from, cur);
            }
            let Some(fid) = c.data_mut().rt.as_mut().expect("rt").slots.remove(&to) else {
                if std::env::var_os("WC_FX").is_some() {
                    eprintln!("[fbr ] MISS {from:#x} -> {to:#x} (cur {cur:#x})");
                }
                return Ok(0);
            };
            Ok(susp_here(Some(Step { fibre: fid, val: from, ctx: None })))
        })?;

    l.func_wrap("env", "fiber_destroy_js", |mut c: Caller<'_, St>, tok: u32| -> Result<()> {
        let Some(fid) = c.data_mut().rt.as_mut().expect("rt").slots.remove(&tok) else {
            return Ok(());
        };
        // Resuming with 0 makes the dead context fall out of `fiber_entry`'s
        // transfer, which the guest turns into `unreachable`; the trap stays
        // inside the fibre closure and ends it.
        let raw = c.data().rt.as_ref().expect("rt").store;
        let mut step = Some(Step { fibre: fid, val: 0, ctx: None });
        while let Some(s) = step {
            step = match resume(raw.get(), s)? {
                Resume::Done => None,
                Resume::Parked(n) => n,
            };
        }
        Ok(())
    })?;
    Ok(())
}

/// Run a host task on its own fibre and guest stack.
pub fn spawn_on_new_stack<T: wasmtime::AsContextMut<Data = St>>(
    cx: &mut T, id: u32, size: u32, task: Task,
) -> Result<()> {
    let size = size.max(64 * 1024) as usize;
    let st = cx.as_context().data().rt.as_ref().expect("rt").store;
    let (sp, base) = guest_stack(cx, size)?;

    let raw = st;
    let stack = FiberStack::new(size, false).map_err(|e| wasmtime::Error::msg(e.to_string()))?;
    let slot: Rc<Cell<*mut Susp>> = Rc::new(Cell::new(std::ptr::null_mut()));
    let ss = slot.clone();
    let fib = Fiber::new(stack, move |_first: u32, s: &mut Susp| {
        ss.set(s as *mut Susp);
        block_on(task);
    })
    .map_err(|(e, _)| wasmtime::Error::msg(e.to_string()))?;
    cx.as_context_mut().data_mut().rt.as_mut().expect("rt").fibres.insert(
        id,
        F { susp: slot, f: Box::new(fib), ctx: Cell::new((sp, sp, base)), guest: (base, size as u32) },
    );
    schedule(st.get(), Step { fibre: id, val: id, ctx: Some((sp, sp, base)) });
    Ok(())
}

/// Run a task on its own fibre and pump until it finishes, timers included.
pub fn call_fibre<T: wasmtime::AsContextMut<Data = St>>(
    cx: &mut T, id: u32, size: u32, task: Task,
) -> Result<()> {
    spawn_on_new_stack(cx, id, size.max(TASK_STACK as u32), task)?;
    let st = cx.as_context().data().rt.as_ref().expect("rt").store.get();
    let mut iters: u64 = 0;
    loop {
        iters += 1;
        if std::env::var_os("WC_HB").is_some() && iters % 3000 == 0 {
            eprintln!("[hb  ] fibre {id:#x} alive, {iters} pump polls, cur={:#x}", st.data().cur);
        }
        if !is_live(st, id) {
            return Ok(());
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
        tick(st);
        if !is_live(st, id) {
            return Ok(());
        }
        if st.data().cur != id && !waiting(st, id) {
            schedule(st, Step { fibre: id, val: id, ctx: None });
        }
    }
}

/// Suspend the caller for `dur`; resumed by `tick`.
pub async fn sleep<T: wasmtime::AsContextMut<Data = St>>(cx: &mut T, dur: std::time::Duration) {
    let id = CUR.with(|c| c.get());
    if id == u32::MAX {
        std::thread::sleep(dur);
        return;
    }
    let st = cx.as_context().data().rt.as_ref().expect("rt").store;
    let ctx = here(st.get()).unwrap_or((0, 0, 0));
    SLEEPERS.with(|q| q.borrow_mut().push((std::time::Instant::now() + dur, id, ctx)));
    susp_here(None);
}

/// Drop the scheduler without waiting for every parked fibre to finish.
pub fn shutdown(store: &mut Store<St>) {
    let Some(rt) = store.data_mut().rt.as_mut() else { return };
    rt.slots.clear();
    for (_, f) in rt.fibres.drain() {
        std::mem::forget(f);
    }
}

/// Whether a fibre is parked under some token, i.e. waiting for a real
/// transfer rather than for the host.
fn waiting(store: &Store<St>, id: u32) -> bool {
    store
        .data()
        .rt
        .as_ref()
        .map(|r| r.slots.values().any(|v| *v == id))
        .unwrap_or(false)
}

pub fn is_live(store: &Store<St>, id: u32) -> bool {
    store.data().rt.as_ref().map(|r| r.fibres.contains_key(&id)).unwrap_or(false)
}

/// Resume every sleeper whose deadline has passed.
pub fn tick(store: &mut Store<St>) {
    let now = std::time::Instant::now();
    let due: Vec<(u32, (u32, u32, u32))> = SLEEPERS.with(|q| {
        let mut q = q.borrow_mut();
        let due = q.iter().filter(|(t, _, _)| *t <= now).map(|(_, i, c)| (*i, *c)).collect();
        q.retain(|(t, _, _)| *t > now);
        due
    });
    for (id, ctx) in due {
        if is_live(store, id) {
            schedule(store, Step { fibre: id, val: id, ctx: Some(ctx) });
        }
    }
}

/// Register the current thread as having no fibre of its own: every piece of
/// guest code has to run inside `call_fibre`.
pub fn host_root(store: &mut Store<St>) {
    STORE.with(|c| c.set(store as *mut Store<St>));
    let rt = store.data_mut().rt.as_mut().expect("rt");
    rt.root = 0;
    rt.store.get().data_mut().root = 0;
    rt.store.get().data_mut().cur = u32::MAX;
    CUR.with(|c| c.set(u32::MAX));
}

/// Register the current (host) stack as the thread root.
pub fn run(store: &mut Store<St>, id: u32) {
    STORE.with(|c| c.set(store as *mut Store<St>));
    let rt = store.data_mut().rt.as_mut().expect("rt");
    rt.root = id;
    rt.store.get().data_mut().root = id;
    rt.store.get().data_mut().cur = id;
    CUR.with(|c| c.set(id));
}

// --- nested-safe block_on --- //
struct Park;
static PVT: RawWakerVTable = RawWakerVTable::new(|_| RawWaker::new(std::ptr::null(), &PVT), |_| {
    std::thread::park();
}, |_| {
    std::thread::park();
}, |_| {});

/// Drive `f` to completion.  On the host root there is no context to park, so
/// the future gets a fibre of its own; on a fibre the poll loop is enough.
pub fn block_on<F: Future>(f: F) -> F::Output
where
    F::Output: 'static,
{
    let st = STORE.with(|c| c.get());
    if CUR.with(|c| c.get()) != u32::MAX || st.is_null() {
        return poll_until(f);
    }

    let out: Rc<RefCell<Option<F::Output>>> = Rc::new(RefCell::new(None));
    let sink = out.clone();
    // SAFETY: we pump to completion before returning, so the caller's borrows
    // in `f` stay live for the whole call.
    let borrowed: Pin<Box<dyn Future<Output = F::Output> + '_>> = Box::pin(f);
    let fut: Pin<Box<dyn Future<Output = F::Output> + 'static>> =
        unsafe { std::mem::transmute(borrowed) };
    let task: Pin<Box<dyn Future<Output = ()> + 'static>> = Box::pin(async move {
        *sink.borrow_mut() = Some(fut.await);
    });

    let id = new_id(unsafe { &mut *st }.data_mut().rt.as_mut().expect("rt"));
    if let Err(e) = spawn_local(unsafe { &mut *st }, id, TASK_STACK, task) {
        panic!("block_on: {e}");
    }
    loop {
        if let Some(v) = out.borrow_mut().take() {
            return v;
        }
        let st = unsafe { &mut *st };
        if !is_live(st, id) {
            match out.borrow_mut().take() {
                Some(v) => return v,
                None => panic!("block_on: task fibre died without a value"),
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
        tick(st);
        if st.data().cur != id && is_live(st, id) && !waiting(st, id) {
            schedule(st, Step { fibre: id, val: id, ctx: None });
        }
    }
}

/// Carve a guest stack for a host task. Before instantiation there is no guest
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
    let p = malloc.call(&mut ccx, (size as i32,))? as u32;
    if p == 0 {
        return Err(wasmtime::Error::msg("guest malloc failed for a task stack"));
    }
    Ok((((p as usize + size) & !15) as u32, p))
}

fn spawn_local(
    st: &mut Store<St>,
    id: u32,
    size: usize,
    fut: Pin<Box<dyn Future<Output = ()> + 'static>>,
) -> Result<()> {
    let (sp, base) = guest_stack(st, size)?;
    let ctx = (sp, sp, base);
    let stack = FiberStack::new(size, false).map_err(|e| wasmtime::Error::msg(e.to_string()))?;
    let slot: Rc<Cell<*mut Susp>> = Rc::new(Cell::new(std::ptr::null_mut()));
    let ss = slot.clone();
    let fib = Fiber::new(stack, move |_first: u32, s: &mut Susp| {
        ss.set(s as *mut Susp);
        let mut fut = fut;
        poll_until(fut.as_mut());
    })
    .map_err(|(e, _)| wasmtime::Error::msg(e.to_string()))?;
    st.data_mut().rt.as_mut().expect("rt").fibres.insert(
        id,
        F { susp: slot, f: Box::new(fib), ctx: Cell::new(ctx), guest: (base, size as u32) },
    );
    schedule(st, Step { fibre: id, val: id, ctx: Some(ctx) });
    Ok(())
}

/// Poll a guest call to completion from sync code.
pub fn run_sync<F: Future>(f: F) -> F::Output {
    poll_until(f)
}

fn poll_until<F: Future>(f: F) -> F::Output {
    let waker = unsafe { Waker::from_raw(RawWaker::new(std::ptr::null(), &PVT)) };
    let mut cx = Context::from_waker(&waker);
    let mut f = Box::pin(f);
    loop {
        if let Poll::Ready(v) = f.as_mut().poll(&mut cx) {
            return v;
        }
        let p = STORE.with(|c| c.get());
        if !p.is_null() {
            tick(unsafe { &mut *p });
        }
        std::thread::park_timeout(std::time::Duration::from_millis(1));
    }
}
