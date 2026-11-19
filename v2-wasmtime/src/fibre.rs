//! The engine's fibre scheduler, on real host stacks.
//!
//! The glue switches fibres by handing the wasm stack pointer around: create
//! saves the caller's sp/base/end under a token and points the stack at the new
//! range; transfer parks the caller under `from` and resumes whoever owns `to`;
//! the value a transfer returns is the key of whoever resumed it.
//!
//! A stackless (future) scheduler cannot do this: a chain A->B->C that comes
//! back to A has to resume *through* the frames in between, and a suspended
//! future's frame is not staying put.  So each guest fibre gets a real host
//! stack (`wasmtime-internal-fiber`) and suspends with `Fiber::suspend`, which
//! is the same primitive the guest expects, minus the wasm sp juggling (the
//! guest still does that itself through `set_stack_ptr`).
use std::cell::Cell;
use std::collections::HashMap;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

use wasmtime::{Caller, Linker, Result, Store};
use wasmtime_internal_fiber::{Fiber, FiberStack, Suspend};

use crate::state::St;

const STACK: usize = 4 << 20;

pub type Task = Pin<Box<dyn Future<Output = ()> + Send + 'static>>;

type Fib = Fiber<'static, u32, u32, ()>;
type Susp = Suspend<u32, u32, ()>;

thread_local! {
    /// suspend handles of the fibres from the root outwards
    static SUSP_STACK: std::cell::RefCell<Vec<*mut Susp>> = const { std::cell::RefCell::new(Vec::new()) };
    /// fibre currently executing on this thread
    static CUR_ID: Cell<u32> = const { Cell::new(u32::MAX) };
    /// timer queue: (deadline, fibre)
    static SLEEPERS: std::cell::RefCell<Vec<(std::time::Instant, u32)>> = const { std::cell::RefCell::new(Vec::new()) };
    /// store of this thread, for the ticker
    static CUR_STORE: Cell<*mut Store<St>> = const { Cell::new(std::ptr::null_mut()) };
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

struct Fibre {
    running: Cell<bool>,
    susp: std::rc::Rc<Cell<*mut Susp>>,
    f: Box<Fib>,
    parent: u32,
    arg: u32,
}

pub struct Rt {
    pub store: PStore,
    /// token -> fibre it belongs to (token 0 is the thread root)
    tokens: HashMap<u32, u32>,
    fibres: HashMap<u32, Fibre>,
    root: u32,
}

unsafe impl Send for Rt {}

impl Rt {
    pub fn new(store: PStore) -> Self {
        Rt { store, tokens: HashMap::new(), fibres: HashMap::new(), root: 0 }
    }
}

fn suspend_here(v: u32) -> u32 {
    let p = SUSP_STACK.with(|c| c.borrow_mut().pop());
    let p = p.expect("suspend outside a fibre");
    let out = unsafe { (*p).suspend(v) };
    SUSP_STACK.with(|c| c.borrow_mut().push(p));
    out
}

fn resume(store: &mut Store<St>, id: u32, v: u32) -> Option<std::result::Result<(), u32>> {
    let prev_cur = store.data().cur;
    let fp: *const Fibre = store.data().rt.as_ref()?.fibres.get(&id)?;
    let f = unsafe { &*fp };
    if f.running.get() || f.f.done() {
        return None;
    }
    f.running.set(true);
    let p = f.susp.get();
    if !p.is_null() {
        SUSP_STACK.with(|c| c.borrow_mut().push(p));
    }
    let prev_id = CUR_ID.with(|c| c.get());
    store.data_mut().cur = id;
    let out = match f.f.resume(v) {
        Ok(()) => Ok(()),
        Err(y) => Err(y),
    };
    store.data_mut().cur = prev_cur;
    CUR_ID.with(|c| c.set(prev_id));
    f.running.set(false);
    Some(out)
}

/// `__asyncjs__create_suspended_fiber_js(id, base, limit, entry, arg) -> resumer`
pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap("env", "__asyncjs__create_suspended_fiber_js",
        |mut c: Caller<'_, St>, id: u32, base: u32, limit: u32, entry: u32, arg: u32| -> Result<u32> {
            crate::trace::hit("fiber_create");
            let cur = c.data().cur;
            eprintln!("[fbr ] create {id:#x} entry={entry} cur={cur:#x}");
            let inst = c.data().inst.expect("instance");
            let raw = c.data().rt.as_ref().expect("rt").store;
            let _ = &mut c;
            let fe = inst.get_typed_func::<(i32, i32, i32), ()>(raw.get(), "fiber_entry")?;
            let _ = (base, limit);

            let stack = FiberStack::new(STACK, false).map_err(|e| wasmtime::Error::msg(e.to_string()))?;
            let susp_slot: std::rc::Rc<Cell<*mut Susp>> = std::rc::Rc::new(Cell::new(std::ptr::null_mut()));
            let ss = susp_slot.clone();
            let fib = Fiber::new(stack, move |_first: u32, s: &mut Susp| {
                ss.set(s as *mut Susp);
                SUSP_STACK.with(|c| c.borrow_mut().push(s as *mut Susp));
                CUR_ID.with(|c| c.set(id));
                let st = raw.get();
                let _ = fe.call(st, (id as i32, entry as i32, arg as i32));
                CUR_ID.with(|c| c.set(u32::MAX));
                SUSP_STACK.with(|c| {
                    c.borrow_mut().pop();
                });
            })
            .map_err(|(e, _)| wasmtime::Error::msg(e.to_string()))?;

            let rt = c.data_mut().rt.as_mut().expect("rt");
            rt.tokens.insert(id, id);
            rt.fibres.insert(id, Fibre { running: Cell::new(false), susp: susp_slot.clone(), f: Box::new(fib), parent: cur, arg });
            rt.store.get().data_mut().cur = id;

            // run it to its opening transfer, which hands control straight back
            let out = match resume(raw.get(), id, cur) {
                Some(Err(v)) => v,
                Some(Ok(())) => 0,
                None => 0,
            };
            eprintln!("[fbr ] create {id:#x} -> {out:#x}");
            c.data_mut().cur = cur;
            Ok(out)
        })?;

    l.func_wrap("env", "__asyncjs__fiber_transfer_js",
        |mut c: Caller<'_, St>, from: u32, to: u32| -> Result<u32> {
            crate::trace::hit("fiber_transfer");
            let cur = c.data().cur;
            eprintln!("[fbr ] xfer {from:#x} -> {to:#x} cur={cur:#x}");
            {
                let rt = c.data_mut().rt.as_mut().expect("rt");
                if from != 0 {
                    rt.tokens.insert(from, cur);
                }
            }

            // Walk to the target, hopping through fibres that only handed the
            // switch on.  `pass` is what the target's suspend returns.
            let mut next = to;
            let mut pass = from;
            loop {
                let t = {
                    let rt = c.data_mut().rt.as_mut().expect("rt");
                    rt.tokens.remove(&next)
                };
                let Some(t) = t else {
                    return Ok(from);
                };
                if t == cur {
                    if cur == 0 && CUR_ID.with(|x| x.get()) == u32::MAX {
                        // the thread root: nobody to switch to
                        return Ok(pass);
                    }
                    // switching to ourselves still parks, so the resumer runs
                    let back = suspend_here(pass);
                    c.data_mut().cur = cur;
                    return Ok(back);
                }
                if t == 0 {
                    // the thread root, below the resume chain: unwind to it
                    let back = suspend_here(pass);
                    c.data_mut().cur = cur;
                    return Ok(back);
                }
                let done = resume(c.data().rt.as_ref().unwrap().store.get(), t, pass);
                match done {
                    Some(Err(v)) => {
                        next = v;
                        pass = v;
                    }
                    Some(Ok(())) => {
                        let rt = c.data_mut().rt.as_mut().expect("rt");
                        rt.fibres.remove(&t);
                        return Ok(pass);
                    }
                    None => return Ok(pass),
                }
            }
        })?;

    let _ = 0;
    l.func_wrap("env", "fiber_destroy_js", |mut c: Caller<'_, St>, id: u32| -> Result<()> {
        let cur = c.data().cur;
        let t = c.data_mut().rt.as_mut().expect("rt").tokens.remove(&id);
        let Some(t) = t else { return Ok(()) };
        if t != cur && t != 0 {
            let done = resume(c.data().rt.as_ref().unwrap().store.get(), t, 0);
            let rt = c.data_mut().rt.as_mut().expect("rt");
            if let Some(Ok(())) = done {
                rt.fibres.remove(&t);
            }
        }
        Ok(())
    })?;
    Ok(())
}

/// Run a task on a private fibre, starting it immediately.
pub fn spawn_on_new_stack<T: wasmtime::AsContextMut<Data = St>>(
    cx: &mut T, id: u32, size: u32, task: Task,
) -> Result<()> {
    let raw = cx.as_context().data().rt.as_ref().expect("rt").store;
    let cur = cx.as_context().data().cur;
    let stack = FiberStack::new(size.max(64 * 1024) as usize, false)
        .map_err(|e| wasmtime::Error::msg(e.to_string()))?;
    let susp_slot: std::rc::Rc<Cell<*mut Susp>> = std::rc::Rc::new(Cell::new(std::ptr::null_mut()));
    let ss = susp_slot.clone();
    let fib = Fiber::new(stack, move |_v: u32, s: &mut Susp| {
        ss.set(s as *mut Susp);
        SUSP_STACK.with(|c| c.borrow_mut().push(s as *mut Susp));
        CUR_ID.with(|c| c.set(id));
        let st = raw.get();
        block_on(task);
        CUR_ID.with(|c| c.set(u32::MAX));
        SUSP_STACK.with(|c| {
            c.borrow_mut().pop();
        });
    })
    .map_err(|(e, _)| wasmtime::Error::msg(e.to_string()))?;
    {
        let mut ctx = cx.as_context_mut();
        let d = ctx.data_mut();
        let rt = d.rt.as_mut().expect("rt");
        rt.tokens.insert(id, id);
        rt.fibres.insert(id, Fibre { running: Cell::new(false), susp: susp_slot.clone(), f: Box::new(fib), parent: cur, arg: 0 });
    }
    cx.as_context_mut().data_mut().cur = id;
    let done = {
        let rt = cx.as_context().data().rt.as_ref().expect("rt");
        resume(raw.get(), id, cur)
    };
    cx.as_context_mut().data_mut().cur = cur;
    if let Some(Ok(())) = done {
        cx.as_context_mut().data_mut().rt.as_mut().unwrap().fibres.remove(&id);
    }
    Ok(())
}

/// Run a task on its own fibre and pump until it finishes, timers included.
pub fn call_fibre<T: wasmtime::AsContextMut<Data = St>>(
    cx: &mut T, id: u32, size: u32, task: Task,
) -> Result<()> {
    spawn_on_new_stack(cx, id, size, task)?;
    let mut n = 0u64;
    loop {
        let (live, raw) = {
            let d = cx.as_context().data();
            (d.rt.as_ref().map(|r| r.fibres.contains_key(&id)).unwrap_or(false),
             d.rt.as_ref().unwrap().store)
        };
        if !live {
            return Ok(());
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
        tick(raw.get());
        {
            let st = raw.get();
            let live = st.data().rt.as_ref().map(|r| r.fibres.contains_key(&id)).unwrap_or(false);
            if live && CUR_ID.with(|c| c.get()) != id {
                let _ = resume(st, id, 0);
            }
        }
        if std::env::var_os("WC_FX").is_some() && (n & 63) == 0 {
            let st = raw.get();
            let d = st.data();
            let m = d.mem.data();
            for (id, f) in d.rt.as_ref().unwrap().fibres.iter() {
                let mut w = String::new();
                for i in 0..8 { w.push_str(&format!(" {:#x}", unsafe { *(m[f.arg as usize + i * 4].get() as *const u32) })); }
                let rec = unsafe { *(m[f.arg as usize + 20].get() as *const u32) };
                let mut r = String::new();
                if rec > 0x1000 { for i in 0..12 { r.push_str(&format!(" {:#x}", unsafe { *(m[rec as usize + i * 4].get() as *const u32) })); } }
                let cell = if rec > 0x1000 { unsafe { *(m[rec as usize + 16].get() as *const u32) } } else { 0 };
                let mut cw = String::new();
                if cell > 0x1000 { for i in 0..4 { cw.push_str(&format!(" {:#x}", unsafe { *(m[cell as usize + i * 4].get() as *const u32) })); } }
                eprintln!("[fx2] fibre {id:#x} rec={rec:#x}:{r} cell={cell:#x}:{cw}");
            }
        }
        n += 1;
    }
}

/// Suspend the caller for `dur`.  Resumed by `tick` from whichever frame is
/// parked, so it must only be used on a fibre.
pub async fn sleep<T: wasmtime::AsContextMut<Data = St>>(cx: &mut T, dur: std::time::Duration) {
    if CUR_ID.with(|c| c.get()) == u32::MAX {
        std::thread::sleep(dur);
        return;
    }
    let id = cx.as_context().data().cur;
    SLEEPERS.with(|q| q.borrow_mut().push((std::time::Instant::now() + dur, id)));
    let _ = suspend_here(id);
}

pub fn is_live(store: &Store<St>, id: u32) -> bool {
    store.data().rt.as_ref().map(|r| r.fibres.contains_key(&id)).unwrap_or(false)
}

/// Resume every sleeper whose deadline has passed.
pub fn tick(store: &mut Store<St>) {
    let now = std::time::Instant::now();
    let due: Vec<u32> = SLEEPERS.with(|q| {
        let mut q = q.borrow_mut();
        let due = q.iter().filter(|(t, _)| *t <= now).map(|(_, i)| *i).collect();
        q.retain(|(t, _)| *t > now);
        due
    });
    if due.is_empty() {
        return;
    }
    let _ = store;
    let cur = CUR_ID.with(|c| c.get());
    let mut ran = false;
    for id in due {
        if id == cur || !store.data().rt.as_ref().map(|r| r.fibres.contains_key(&id)).unwrap_or(false) {
            SLEEPERS.with(|q| q.borrow_mut().push((now, id)));
            continue;
        }
        let _ = resume(store, id, id);
    }
}

/// Register the current (host) stack as the thread root, so transfers to token
/// 0 have somewhere to go.
pub fn run(store: &mut Store<St>, id: u32) {
    CUR_STORE.with(|c| c.set(store as *mut Store<St>));
    let rt = store.data_mut().rt.as_mut().expect("rt");
    rt.root = id;
    rt.store.get().data_mut().root = id;
    rt.store.get().data_mut().cur = id;
}

// --- nested-safe block_on --- //
struct Park;
static PVT: RawWakerVTable = RawWakerVTable::new(|_| RawWaker::new(std::ptr::null(), &PVT), |_| {
    std::thread::park();
}, |_| {
    std::thread::park();
}, |_| {});

pub fn block_on<F: Future>(f: F) -> F::Output {
    let waker = unsafe { Waker::from_raw(RawWaker::new(std::ptr::null(), &PVT)) };
    let mut cx = Context::from_waker(&waker);
    let mut f = Box::pin(f);
    loop {
        if let Poll::Ready(v) = f.as_mut().poll(&mut cx) {
            return v;
        }
        // drive timers while parked; the waker unparks us early
        let p = CUR_STORE.with(|c| c.get());
        if !p.is_null() {
            tick(unsafe { &mut *p });
        }
        std::thread::park_timeout(std::time::Duration::from_millis(1));
    }
}
