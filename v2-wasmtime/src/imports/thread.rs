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
                crate::imports::gl::register(&mut l)?;
                crate::imports::opfs::register(&mut l)?;
                crate::imports::rna::register(&mut l)?;
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

                // the page's worker order: thread init, TLS, mailbox arm,
                // then the instance's static initialisers on the worker's own
                // stack (the arm happens inside them)
                let ti = inst.get_typed_func::<(i32, i32, i32, i32, i32, i32), ()>(
                    &mut *store, "_emscripten_thread_init")?;
                let async_thread = std::env::var_os("WC_WASYNC").is_some();
                ti.call(&mut *store, (pthread as i32, async_thread as i32, 0, 1, 0, 0))?;
                let tls = inst.get_typed_func::<(), (i32,)>(&mut *store, "_emscripten_tls_init")?;
                tls.call(&mut *store, ())?;
                if std::env::var_os("WC_ST").is_some() { eprintln!("[st  ] {pthread:x} init+tls done"); }
                static EMBIND: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
                if !EMBIND.swap(true, std::sync::atomic::Ordering::Relaxed) {
                    if let Ok(b) = inst.get_typed_func::<(), ()>(&mut *store, "_embind_initialize_bindings") {
                        let _ = b.call(&mut *store, ());
                    }
                }
                // the glue arms each worker's mailbox (`Er`) right after TLS:
                // mark it ready and remember it so rings can be drained here
                store.data_mut().mbx_ptr = Some(pthread);
                unsafe {
                    *(store.data().mem.data()[pthread as usize + 112].get() as *mut u32) = 1
                };
                if std::env::var_os("WC_WASYNC").is_some() {
                    let sts = inst.get_typed_func::<(i32, i32, i32, i32), ()>(&mut *store, "__set_thread_state")?;
                    sts.call(&mut *store, (pthread as i32, 1, 0, 0))?;
                    let a = unsafe { *(store.data().mem.data()[9515280].get() as *const u32) };
                    eprintln!("[st  ] {pthread:x} async=1 depth={a}");
                }
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
                let raw = f.to_raw(&mut *store);
                let lib = inst.get_func(&mut *store, "main_loop_iteration").map(|g| g.to_raw(&mut *store) as usize);
                let ma = inst.get_func(&mut *store, "main_loop_iteration").map(|g| unsafe { *(g.to_raw(&mut *store) as *const usize) });
                let code = unsafe { *((raw as *const usize).add(1)) };
                eprintln!("[pthr] {pthread:x} start slot={start} code={code:x} raw={raw:?} (lib raw={lib:x?} libcode={ma:x?})");
                crate::fibre::run(&mut *store, 0);
                let ex = inst.get_typed_func::<(i32,), ()>(&mut *store, "_emscripten_thread_exit").ok();
                let p = store.data().rt.as_ref().unwrap().store;
                let ctors = inst.get_typed_func::<(), ()>(&mut *store, "__wasm_call_ctors")?;
                let iter = inst.get_typed_func::<(), ()>(&mut *store, "main_loop_iteration").ok();
                let task: crate::fibre::Task = Box::pin(async move {
                    let st = p.get();
                    // workers do not run ctors on the page (`pn` is
                    // `!isPthread && ctors`); statics are main-thread work.
                    if std::env::var_os("WC_WCTORS").is_some() {
                        if let Err(e) = ctors.call(&mut *st, ()) {
                            eprintln!("[pthr] ctors {pthread:x} threw: {e:#}");
                        }
                    }
                    let mut out = [Val::I32(0)];
                    let mut unwound = false;
                    let mut returned = false;
                    match f.call(&mut *st, &[Val::I32(arg as i32)], &mut out) {
                        Err(e) if was_unwind(&e) => unwound = true,
                        Err(e) => eprintln!("[pthr] entry {pthread:x} threw: {e:#}"),
                        Ok(()) => {
                            if std::env::var_os("WC_PTHR").is_some() {
                                eprintln!("[pthr] entry {pthread:x} returned");
                            }
                            // keep the pthread addressable: a routine that
                            // returns still gets rings dispatched to it on the
                            // page (the worker is only pooled, not freed)
                            let rc = match out[0] { Val::I32(v) => v, _ => 0 };
                            if std::env::var_os("WC_PEXIT").is_some() {
                                if let Some(ex) = &ex {
                                    let _ = ex.call(&mut *st, (rc,));
                                }
                            } else {
                                unwound = true;
                                returned = true;
                            }
                        }
                    }
                    // Unwinding to the event loop leaves the thread alive on
                    // the page: the JS loop keeps it around for mailbox work,
                    // timers and (if it asked) its own main loop.  Do the same.
                    if unwound {
                        loop {
                            crate::fibre::sleep(&mut *st, std::time::Duration::from_millis(1)).await;
                            let _ = crate::imports::mailbox::pump_mailbox(&mut *st).await;
                            if let (Some(iter), false) = (&iter, returned) {
                                if let Err(e) = iter.call(&mut *st, ()) {
                                    if !was_unwind(&e) {
                                        eprintln!("[pthr] loop {pthread:x} threw: {e:#}");
                                    }
                                }
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

/// `emscripten_unwind_to_js_event_loop` / `exit_with_live_runtime`.
fn was_unwind(e: &wasmtime::Error) -> bool {
    e.to_string().contains("unwind")
}

pub fn register(l: &mut Linker<St>) -> Result<()> {
    l.func_wrap("env", "__pthread_create_js",
        |c: Caller<'_, St>, pthread: i32, _attr: i32, start: i32, arg: i32| -> i32 {
            let (Some(engine), Some(module)) = (c.data().engine.clone(), c.data().module.clone())
            else {
                return 6;
            };
            if std::env::var_os("WC_PTHR").is_some() {
                eprintln!("[pthc] {} start_slot={start} arg={arg:#x}", std::thread::current().name().unwrap_or("?"));
            }
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
