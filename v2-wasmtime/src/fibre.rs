//! The engine's fibre scheduler, ported from the page glue.
//!
//! The glue switches fibres by handing the wasm stack pointer around:
//! `create_suspended_fiber_js(id, top, bottom, fn, arg)` saves the caller's
//! sp/base/end under `id`, points the stack at the new range and calls
//! `fiber_entry(id, fn, arg)`; `fiber_transfer_js(from, to)` saves `from`'s
//! state, restores `to`'s and resolves `to`'s pending promise with `from`.
//! Both imports return the id of whoever resumed them, so the return value of
//! a create/transfer is the resumer, not a status.
//!
//! Chrome gets the stackful part from JSPI: every suspended import keeps its
//! wasm frames alive. wasmtime is stackless, so this module keeps the wasm
//! calls themselves alive as futures in a tiny executor, switches the wasm
//! stack pointer on the way in and out, and polls them here. The wasm frames
//! live on the wasm stacks (that is why the glue moves the pointer), so a
//! suspended call resumes exactly where it stopped.
use std::collections::{HashMap, VecDeque};
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Condvar, Mutex};
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

use futures::channel::oneshot;
use wasmtime::{AsContextMut, Caller, Linker, Result, Store};

use crate::state::St;

pub type Task = Pin<Box<dyn Future<Output = ()> + Send + 'static>>;

struct Fiber {
    sp: u32,
    base: u32,
    end: u32,
    resume: Option<oneshot::Sender<u32>>,
    task: Option<Task>,
}

type Q = Arc<(Mutex<VecDeque<u32>>, Condvar)>;

/// Raw store handle; only ever touched from the owning thread.
#[derive(Clone, Copy)]
pub struct PStore(pub *mut Store<St>);

unsafe impl Send for PStore {}

impl PStore {
    pub fn get(&self) -> &'static mut Store<St> {
        unsafe { &mut *self.0 }
    }
}

pub struct Rt {
    pub store: PStore,
    q: Q,
    fibers: HashMap<u32, Fiber>,
    alive: usize,
    /// deepest fibre stack currently installed (restored on wake)
    root: u32,
}

// single-threaded: only the owning thread touches the store
unsafe impl Send for Rt {}

impl Rt {
    pub fn new(store: PStore) -> Self {
        Rt {
            store,
            q: Arc::new((Mutex::new(VecDeque::new()), Condvar::new())),
            fibers: HashMap::new(),
            alive: 0,
            root: 0,
        }
    }

    fn push(&self, id: u32) {
        let (m, cv) = &*self.q;
        m.lock().unwrap().push_back(id);
        cv.notify_one();
    }
}

// --- waker --- //
struct Wake {
    id: u32,
    q: Q,
}

static VT: RawWakerVTable = RawWakerVTable::new(clone, wake, wake_ref, drop_w);

unsafe fn clone(p: *const ()) -> RawWaker {
    Arc::increment_strong_count(p as *const Wake);
    RawWaker::new(p, &VT)
}

unsafe fn wake(p: *const ()) {
    let w = Arc::from_raw(p as *const Wake);
    let (m, cv) = &*w.q;
    m.lock().unwrap().push_back(w.id);
    cv.notify_one();
}

unsafe fn wake_ref(p: *const ()) {
    let w = &*(p as *const Wake);
    let (m, cv) = &*w.q;
    m.lock().unwrap().push_back(w.id);
    cv.notify_one();
}

unsafe fn drop_w(p: *const ()) {
    drop(Arc::from_raw(p as *const Wake));
}

fn waker_for(rt: &Rt, id: u32) -> Waker {
    let w = Arc::new(Wake { id, q: rt.q.clone() });
    unsafe { Waker::from_raw(RawWaker::new(Arc::into_raw(w) as *const (), &VT)) }
}

// --- stack helpers (exports on the module) --- //
async fn sp_of<T: AsContextMut<Data = St>>(cx: &mut T) -> Result<u32> {
    let inst = cx.as_context().data().inst.expect("instance");
    let f = inst.get_typed_func::<(), (u32,)>(&mut *cx, "get_stack_ptr")?;
    Ok(f.call_async(&mut *cx, ()).await?.0)
}

async fn set_sp<T: AsContextMut<Data = St>>(cx: &mut T, sp: u32) -> Result<()> {
    let inst = cx.as_context().data().inst.expect("instance");
    let f = inst.get_typed_func::<(u32,), ()>(&mut *cx, "set_stack_ptr")?;
    f.call_async(&mut *cx, (sp,)).await
}

async fn set_limits<T: AsContextMut<Data = St>>(cx: &mut T, base: u32, end: u32) -> Result<()> {
    let inst = cx.as_context().data().inst.expect("instance");
    let f = inst.get_typed_func::<(u32, u32), ()>(&mut *cx, "set_stack_limits")?;
    f.call_async(&mut *cx, (base, end)).await
}

/// Install fibre `id`'s stack as the current one.
async fn enter<T: AsContextMut<Data = St>>(cx: &mut T, id: u32) -> Result<()> {
    let (base, end, sp) = {
        let rt = cx.as_context().data().rt.as_ref().expect("rt");
        let f = rt.fibers.get(&id).expect("fibre");
        (f.base, f.end, f.sp)
    };
    set_limits(cx, base, end).await?;
    set_sp(cx, sp).await
}

// --- imports --- //

/// `__asyncjs__create_suspended_fiber_js(id, base, limit, entry, arg) -> resumer`
pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap_async("env", "__asyncjs__create_suspended_fiber_js",
        |mut c: Caller<'_, St>, (id, base, limit, entry, arg): (u32, u32, u32, u32, u32)|
        Box::new(async move {
            crate::trace::hit("fiber_create");
            let cur = c.data().cur;
            let sp = sp_of(&mut c).await?;

            let (tx, rx) = oneshot::channel();
            let task = spawn_entry(&mut c, id, entry, arg, base, limit).await?;
            {
                let rt = c.data_mut().rt.as_mut().expect("rt");
                let me = rt.fibers.get_mut(&cur).expect("current fibre");
                me.sp = sp;
                rt.fibers.insert(id, Fiber { sp: base, base, end: limit, resume: Some(tx), task: Some(task) });
                rt.alive += 1;
                rt.push(id);
            }

            // run the new fibre; return whoever sends us back
            Ok(rx.await.unwrap_or(0))
        }))?;

    l.func_wrap_async("env", "__asyncjs__fiber_transfer_js",
        |mut c: Caller<'_, St>, (from, to): (u32, u32)| Box::new(async move {
            crate::trace::hit("fiber_transfer");
            if from == to {
                return Ok(from);
            }
            let sp = sp_of(&mut c).await?;
            let (tx, rx) = oneshot::channel();

            let next = {
                let rt = c.data_mut().rt.as_mut().expect("rt");
                if from != 0 {
                    if let Some(f) = rt.fibers.get_mut(&from) {
                        f.sp = sp;
                        f.resume = Some(tx);
                    } else {
                        return Ok(from);
                    }
                }
                let target = rt.fibers.get_mut(&to).ok_or_else(|| {
                    wasmtime::Error::msg(format!("transfer to unknown fibre {to:#x}"))
                })?;
                let sender = target.resume.take();
                let (base, end, sp) = (target.base, target.end, target.sp);
                (sender, base, end, sp)
            };
            let (sender, base, end, sp) = next;
            set_limits(&mut c, base, end).await?;
            set_sp(&mut c, sp).await?;
            if let Some(s) = sender {
                let _ = s.send(from);
            }
            let rt = c.data().rt.as_ref().expect("rt");
            rt.push(to);

            Ok(rx.await.unwrap_or(from))
        }))?;

    // the fibre's entry call, kept alive as a task
    l.func_wrap_async("env", "fiber_destroy_js",
        |_c: Caller<'_, St>, (_id,): (u32,)| Box::new(async move { Ok(()) }))?;
    Ok(())
}

async fn spawn_entry<T: AsContextMut<Data = St>>(
    cx: &mut T, id: u32, entry: u32, arg: u32, base: u32, limit: u32,
) -> Result<Task> {
    let inst = cx.as_context().data().inst.expect("instance");
    let store = cx.as_context().data().rt.as_ref().expect("rt").store;
    let f = inst.get_table(&mut *cx, "__indirect_function_table").ok_or_else(|| {
        wasmtime::Error::msg("no table")
    })?;
    let slot = f.get(&mut *cx, entry as u64).ok_or_else(|| {
        wasmtime::Error::msg(format!("fibre entry {entry} missing"))
    })?;
    let func = match slot {
        wasmtime::Ref::Func(Some(f)) => f,
        _ => return Err(wasmtime::Error::msg("fibre entry is not a func")),
    };
    let _ = (base, limit);
    Ok(Box::pin(async move {
        let mut out = [];
        if let Err(e) = func.call_async(store.get(), &[wasmtime::Val::I32(id as i32),
                                                       wasmtime::Val::I32(arg as i32)], &mut out).await {
            eprintln!("[fibre] entry {id:#x} threw: {e:#}");
        }
        if let Some(rt) = store.get().data_mut().rt.as_mut() {
            rt.alive = rt.alive.saturating_sub(1);
            rt.fibers.remove(&id);
        }
    }))
}

/// Register the calling thread's own stack as a fibre and run its task.
pub fn run(store: &mut Store<St>, id: u32, root: Task) -> Result<usize> {
    let cur = block_on_inline(sp_of(store))?;
    let (base, end) = {
        let inst = store.data().inst.expect("instance");
        let b = inst.get_typed_func::<(), (u32,)>(&mut *store, "get_stack_base")?;
        let e = inst.get_typed_func::<(), (u32,)>(&mut *store, "get_stack_end")?;
        (block_on_inline(b.call_async(&mut *store, ()))?.0,
         block_on_inline(e.call_async(&mut *store, ()))?.0)
    };
    {
        let rt = store.data_mut().rt.as_mut().expect("rt");
        rt.root = id;
        rt.alive += 1;
        rt.fibers.insert(id, Fiber { sp: cur, base, end, resume: None, task: Some(root) });
        rt.push(id);
    }
    pump(store)
}

/// Run `task` as a fibre on a freshly allocated wasm stack.
pub fn spawn_on_new_stack(store: &mut Store<St>, id: u32, size: u32, task: Task) -> Result<()> {
    let inst = store.data().inst.expect("instance");
    let alloc = inst.get_typed_func::<(u32,), u32>(&mut *store, "emscripten_stack_alloc")?;
    let low = block_on_inline(alloc.call_async(&mut *store, (size,)))?;
    let base = low + size;
    {
        let rt = store.data_mut().rt.as_mut().expect("rt");
        rt.alive += 1;
        rt.fibers.insert(id, Fiber { sp: base, base, end: low, resume: None, task: Some(task) });
        rt.push(id);
    }
    Ok(())
}

/// Suspend the calling fibre for `dur`.
pub async fn sleep<T: AsContextMut<Data = St>>(cx: &mut T, dur: std::time::Duration) {
    let (tx, rx) = oneshot::channel::<()>();
    std::thread::spawn(move || {
        std::thread::sleep(dur);
        let _ = tx.send(());
    });
    let _ = rx.await;
    let _ = cx;
}

pub fn pump(store: &mut Store<St>) -> Result<usize> {
    loop {
        let next = {
            let rt = store.data().rt.as_ref().expect("rt");
            let (m, cv) = &*rt.q;
            let mut g = m.lock().unwrap();
            if g.is_empty() {
                if rt.alive == 0 {
                    return Ok(0);
                }
                let (g2, _) = cv.wait_timeout(g, std::time::Duration::from_millis(4)).unwrap();
                g = g2;
            }
            g.pop_front()
        };
        let Some(id) = next else {
            return Ok(store.data().rt.as_ref().map(|r| r.alive).unwrap_or(0));
        };

        let mut task = match store.data_mut().rt.as_mut().and_then(|r| r.fibers.get_mut(&id)).and_then(|f| f.task.take()) {
            Some(t) => t,
            None => continue,
        };
        let wk = {
            let rt = store.data().rt.as_ref().expect("rt");
            waker_for(rt, id)
        };
        let mut cx = Context::from_waker(&wk);

        {
            let st = store.data().rt.as_ref().expect("rt").store.get();
            let (base, end, sp) = {
                let rt = st.data().rt.as_ref().expect("rt");
                match rt.fibers.get(&id) {
                    Some(f) => (f.base, f.end, f.sp),
                    None => (0, 0, 0),
                }
            };
            if base != 0 {
                block_on_inline(set_limits(st, base, end))?;
                block_on_inline(set_sp(st, sp))?;
            }
        }

        if let Poll::Pending = task.as_mut().poll(&mut cx) {
            if let Some(f) = store.data_mut().rt.as_mut().and_then(|r| r.fibers.get_mut(&id)) {
                f.task = Some(task);
            }
        }
    }
}

fn block_on_inline<F: Future>(f: F) -> F::Output {
    futures::executor::block_on(f)
}
