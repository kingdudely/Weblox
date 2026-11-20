//! Worker threads: one Store + instance per thread over the shared memory,
//! the way the browser's per-worker module instance works.
use anyhow::{anyhow, Result};
use futures::executor::block_on;
use wasmtime::{Caller, Extern, Linker, Store, Val};

use crate::state::St;

fn start_worker(
    engine: wasmtime::Engine,
    module: wasmtime::Module,
    mem: wasmtime::SharedMemory,
    pthread: u32,
    start: u32,
    arg: u32,
) -> Result<()> {
    crate::diag::worker_up();
    std::thread::Builder::new()
        .name(format!("rbx-{pthread:x}"))
        .spawn(move || -> Result<()> {
            let r = block_on(async move {
                let mut store = Box::new(Store::new(&engine, St::new(mem.clone())));
                let p = crate::fibre::PStore(&mut *store as *mut Store<St>);
                store.data_mut().rt = Some(crate::fibre::Rt::new(p));
                store.data_mut().engine = Some(engine.clone());
                store.data_mut().module = Some(module.clone());
                let mut l = Linker::new(&engine);
                crate::imports::embind::register(&mut l)?;
                crate::imports::boot::register(&mut l)?;
                crate::imports::fetch::register(&mut l)?;
                crate::imports::http::register(&mut l)?;
                crate::fibre::register(&mut l)?;
                crate::imports::futex::register(&mut l)?;
                crate::imports::mailbox::register(&mut l)?;
                crate::imports::stubs::register(&mut l)?;
                crate::imports::thread::register(&mut l)?;
                l.define(&mut *store, "env", "memory", Extern::SharedMemory(mem))?;
                eprintln!("[pthr] {pthread:x} instantiating");
                let inst = l.instantiate_async(&mut *store, &module).await?;
                store.data_mut().inst = Some(inst);
                if std::env::var_os("WC_ST").is_some() { eprintln!("[st  ] {pthread:x} instantiated"); }
                if let Ok(v) = std::env::var("WC_SLOT0") {
                    if let Ok(k) = v.parse::<u64>() {
                        let t = inst.get_table(&mut *store, "__indirect_function_table").unwrap();
                        if let Some(f) = t.get(&mut *store, k) {
                            t.set(&mut *store, 0, f)?;
                        }
                    }
                }

                // the parent allocated the stack: +48 is its top, +52 its size
                let memh = store.data().mem.clone();
                let at = |i: usize| unsafe {
                    *(memh.data()[pthread as usize + i].get() as *const u32)
                };
                let top = at(48);
                let size = at(52);
                let lim = inst.get_typed_func::<(i32, i32), ()>(&mut *store, "set_stack_limits")?;
                lim.call(&mut *store, (top as i32, (top - size) as i32))?;
                let sp = inst.get_typed_func::<(i32,), ()>(&mut *store, "set_stack_ptr")?;
                sp.call(&mut *store, (top as i32,))?;

                // per-instance TLS first, then the instance's own static
                // initialisers, as the page does for every worker
                let tls = inst.get_typed_func::<(), (i32,)>(&mut *store, "_emscripten_tls_init")?;
                tls.call(&mut *store, ())?;
                if std::env::var_os("WC_ST").is_some() { eprintln!("[st  ] {pthread:x} tls done"); }
                // give the thread its id and let its futexes use the host path
                // before any static initialiser runs
                let sts0 = inst.get_typed_func::<(i32, i32, i32, i32), ()>(&mut *store, "__set_thread_state")?;
                sts0.call(&mut *store, (pthread as i32, 1, 0, 0))?;
                if std::env::var_os("WC_WCTORS").is_some() {
                    inst.get_typed_func::<(), ()>(&mut *store, "__wasm_call_ctors")?
                        .call(&mut *store, ())?;
                }
                if std::env::var_os("WC_ST").is_some() { eprintln!("[st  ] {pthread:x} ctors done"); }
                let ti = inst.get_typed_func::<(i32, i32, i32, i32, i32, i32), ()>(
                    &mut *store, "_emscripten_thread_init")?;
                ti.call(&mut *store, (pthread as i32, 0, 0, 1, 0, 0))?;
                if std::env::var_os("WC_ST").is_some() {
                    let m = store.data().mem.clone();
                    let w = |o: usize| unsafe { *(m.data()[pthread as usize + o].get() as *const u32) };
                    eprintln!("[st  ] {pthread:x} after init: +0={:#x} +8={:#x} +12={:#x} +16={:#x}", w(0), w(8), w(12), w(16));
                }
                if std::env::var_os("WC_NOSTS").is_none() {
                    let sts = inst.get_typed_func::<(i32, i32, i32, i32), ()>(&mut *store, "__set_thread_state")?;
                    sts.call(&mut *store, (pthread as i32, 0, 0, 0))?;
                }
                static EMBIND: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
                if !EMBIND.swap(true, std::sync::atomic::Ordering::Relaxed) {
                    if let Ok(b) = inst.get_typed_func::<(), ()>(&mut *store, "_embind_initialize_bindings") {
                        let _ = b.call(&mut *store, ());
                    }
                }
                unsafe {
                    *(store.data().mem.data()[pthread as usize + 112].get() as *mut u32) = 1
                };
                let (f0, f104, f112) = (at(0), at(104), at(112));
                eprintln!("[pthr] {pthread:x} fields: +0={f0:#x} +104={f104:#x} +112={f112:#x}");

                let table = inst
                    .get_table(&mut *store, "__indirect_function_table")
                    .ok_or_else(|| anyhow!("no table"))?;
                let slot = table
                    .get(&mut *store, start as u64)
                    .ok_or_else(|| anyhow!("no start fn"))?;
                let f = match slot {
                    wasmtime::Ref::Func(Some(f)) => f,
                    _ => return Err(anyhow!("start routine is not a func")),
                };
                crate::fibre::run(&mut *store, 0);
                let ex = inst.get_typed_func::<(i32,), ()>(&mut *store, "_emscripten_thread_exit").ok();
                let p = store.data().rt.as_ref().unwrap().store;
                let task: crate::fibre::Task = Box::pin(async move {
                    let st = p.get();
                    let mut out = [Val::I32(0)];
                    match f.call(&mut *st, &[Val::I32(arg as i32)], &mut out) {
                        Err(e) => eprintln!("[pthr] entry {pthread:x} threw: {e:#}"),
                        Ok(()) => {
                            let rc = match out[0] { Val::I32(v) => v, _ => 0 };
                            if let Some(ex) = ex {
                                let _ = ex.call(&mut *st, (rc,));
                            }
                        }
                    }
                });
                crate::fibre::call_fibre(&mut *store, 0, 4 * 1024 * 1024, task)?;
                crate::fibre::shutdown(&mut *store);
                crate::diag::worker_down();
                eprintln!("[pthr] worker {pthread:x} finished");
                Ok(())
            });
            if let Err(e) = &r {
                eprintln!("[pthr] worker {pthread:x} failed: {e:#}");
            }
            r
        })?;
    eprintln!("[pthr] spawned {pthread:x} start={start} arg={arg}");

    Ok(())
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap("env", "__pthread_create_js",
        |c: Caller<'_, St>, pthread: i32, _attr: i32, start: i32, arg: i32| -> i32 {
            let (Some(engine), Some(module)) = (c.data().engine.clone(), c.data().module.clone())
            else {
                return 6;
            };
            match start_worker(engine, module, c.data().mem.clone(), pthread as u32,
                               start as u32, arg as u32) {
                Ok(()) => 0,
                Err(e) => {
                    eprintln!("[pthr] spawn failed: {e:#}");
                    11
                }
            }
        })?;
    Ok(())
}
