//! Headless host for Roblox's WebClient.wasm on wasmtime.
//!
//! The engine is the stock browser module; this process supplies what Chrome
//! and WebClient.js supply: a shared memory, the `env` imports, the embind
//! bootstrap, and an event loop.  Suspending imports run as wasmtime async
//! host functions, which is the same suspend/resume the browser gets from
//! `WebAssembly.Suspending`.
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use futures::executor::block_on;
use wasmtime::*;

mod binds;
mod diag;
mod fibre;
mod trace;
mod imports;
mod state;

use state::{St, CANVAS_H, CANVAS_W};

const WASM: &str = "/home/john/rr/WebClient.wasm";
const CACHE: &str = "data/cache/webclient.cwasm";
const CACHE_STAMP: &str = "data/cache/webclient.stamp";
const PAGES: u32 = 32768; // 2 GiB, as the page does (FIntWasmMemoryPages)
const WEB_BASE: &str = "https://www.roblox.com/";
const ASSET_BASE: &str =
    "https://setup.rbxcdn.com/webclient/version-f807b21caba84809-RobloxWebClient/";

fn config() -> Result<Config> {
    let mut c = Config::new();
    c.wasm_threads(true);
    c.shared_memory(true);
    c.async_support(true);
    c.wasm_tail_call(true);
    c.wasm_simd(true);
    c.wasm_relaxed_simd(true);
    c.wasm_bulk_memory(true);
    c.wasm_reference_types(true);
    c.wasm_multi_value(true);
    c.wasm_exceptions(true);
    c.cranelift_opt_level(OptLevel::None); // development profile
    c.parallel_compilation(true);
    c.wasm_backtrace_details(WasmBacktraceDetails::Enable);
    Ok(c)
}

fn stamp() -> String {
    let md = std::fs::metadata(WASM).map(|m| (m.len(), m.modified().ok())).ok();
    format!("{:?}", md)
}

fn module(engine: &Engine) -> Result<Module> {
    let st = stamp();
    if let Ok(prev) = std::fs::read_to_string(CACHE_STAMP) {
        if prev == st {
            let buf = std::fs::read(CACHE)?;
            return Ok(unsafe { Module::deserialize(engine, &buf) }?);
        }
    }
    eprintln!("[wasm] compiling {WASM} (cached builds are instant after this)");
    let bytes = std::fs::read(WASM)?;
    let t = Instant::now();
    let pre = engine.precompile_module(&bytes)?;
    eprintln!("[wasm] compiled in {} ms ({} bytes)", t.elapsed().as_millis(), pre.len());
    std::fs::create_dir_all("data/cache")?;
    std::fs::write(CACHE, &pre)?;
    std::fs::write(CACHE_STAMP, st)?;
    Ok(unsafe { Module::deserialize(engine, &pre) }?)
}

fn main() {
    if let Err(e) = run() {
        eprintln!("[fatal] {e:#}");
        let mut src = e.source();
        while let Some(s) = src {
            eprintln!("[fatal]   caused by: {s}");
            src = s.source();
        }
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let engine = Engine::new(&config()?)?;
    let module = module(&engine)?;
    let mem = SharedMemory::new(&engine, MemoryType::shared(PAGES, PAGES))?;

    let mut linker = Linker::new(&engine);
    imports::embind::register(&mut linker)?;
    imports::boot::register(&mut linker)?;
    imports::fetch::register(&mut linker)?;
    imports::futex::register(&mut linker)?;
    fibre::register(&mut linker)?;
    imports::mailbox::register(&mut linker)?;
    imports::stubs::register(&mut linker)?;
    imports::thread::register(&mut linker)?;

    let mut store = Box::new(Store::new(&engine, St::new(mem.clone())));
    let sp = fibre::PStore(&mut *store as *mut Store<St>);
    store.data_mut().rt = Some(fibre::Rt::new(sp));
    store.data_mut().engine = Some(engine.clone());
    diag::start();
    store.data_mut().is_main = true;
    store.data_mut().module = Some(module.clone());
    linker.define(&mut *store, "env", "memory", Extern::SharedMemory(mem))?;
    let inst = block_on(linker.instantiate_async(&mut *store, &module))?;
    store.data_mut().inst = Some(inst);

    let s: &mut Store<St> = &mut store;
    let ctors = inst.get_typed_func::<(), ()>(&mut *s, "__wasm_call_ctors")?;
    if std::env::var_os("WC_NOCtors").is_none() {
    if let Err(e) = block_on(ctors.call_async(s, ())) {
        eprintln!("[fatal] ctors threw; registered {} bindings so far", store.data().binds.len());
        dump_exception(&mut *store);
        trace::dump();
        return Err(e.into());
    }
    eprintln!("[wasm] ctors ok, {} bindings registered", store.data().binds.len());
    } else { eprintln!("[wasm] ctors skipped, {} binds pre-existing", store.data().binds.len()); }
    block_on(prepare(&mut store, &inst))?;

    let main_fn = inst.get_typed_func::<(i32, i32), i32>(&mut *store, "__main_argc_argv")?;
    let argv = block_on(guest_argv(&mut *store, &inst))?;
    let rc = match block_on(main_fn.call_async(&mut *store, (1, argv as i32))) {
        Ok(r) => r,
        Err(_) if store.data().exited => {
            eprintln!("[wasm] main exited to live runtime");
            0
        }
        Err(e) => {
            eprintln!("[fatal] main trapped: {e:#}");
            dump_exception(&mut *store);
        trace::dump();
            eprintln!(
                "[fatal] binds={} assets={} files={} preload={}",
                store.data().binds.len(),
                store.data().assets.len(),
                store.data().files.len(),
                store.data().preload.len()
            );
            return Err(e.into());
        }
    };
    eprintln!("[wasm] main returned {rc}");

    let iter = inst.get_typed_func::<(), ()>(&mut *store, "main_loop_iteration")?;
    let secs: f64 = std::env::var("WC_LOOP_MS").ok().and_then(|v| v.parse().ok()).unwrap_or(60_000.0);
    let deadline = Instant::now() + Duration::from_secs_f64(secs / 1000.0);
    std::thread::spawn(|| diag::watch(3));
    let mut turns = 0u64;
    let mut root_id = 0xFFFF_0000u32;
    while Instant::now() < deadline {
        block_on(imports::mailbox::drop_in(&mut *store));
        if turns % 500 == 0 {
            eprintln!("[iter] turn {turns}");
        }
        turns += 1;
        root_id += 1;

        let p = store.data().rt.as_ref().expect("rt").store;
        let it = iter.clone();
        let task: fibre::Task = Box::pin(async move {
            let st = p.get();
            if let Err(e) = it.call_async(st, ()).await {
                // the glue catches a throw per turn and keeps the loop alive
                static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
                let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                if n < 5 {
                    eprintln!("[iter] threw: {e:#}");
                }
            }
        });
        fibre::run(&mut *store, root_id, task)?;
        diag::checkpoint();
        std::thread::sleep(Duration::from_millis(1));
    }
    eprintln!("[wasm] {turns} turns; last event {}", diag::last());
    trace::dump();
    eprintln!("[wasm] loop done");
    Ok(())
}

fn dump_exception(store: &mut Store<St>) {
    let Some(e) = store.take_pending_exception() else {
        return;
    };
    match e.tag(&mut *store) {
        Ok(tag) => {
            let ty = tag.ty(&mut *store).clone();
            eprintln!("[exn ] tag={:?}", tag);
            eprintln!("[exn ] type={ty:?}");
            for (i, p) in ty.ty().params().enumerate() {
                if let Ok(v) = e.field(&mut *store, i) {
                    eprintln!("[exn ]   [{i}] {p:?} = {v:?}");
                    if let Val::I32(ptr) = v {
                        let d = store.data().mem.data();
                        let base = ptr as usize;
                        let mut run = String::new();
                        for k in 0..256usize {
                            let b = unsafe { *d[base + k].get() };
                            if (0x20..0x7f).contains(&b) {
                                run.push(b as char);
                            } else {
                                if run.len() >= 4 {
                                    eprintln!("[exn ]     str @+{k}: {run:?}");
                                }
                                run.clear();
                            }
                        }
                    }
                }
            }
        }
        Err(err) => eprintln!("[exn ] no tag: {err}"),
    }
}

async fn guest_argv(store: &mut Store<St>, inst: &Instance) -> Result<u32> {
    let malloc = inst.get_typed_func::<(i32,), i32>(&mut *store, "malloc")?;
    let p = malloc.call_async(&mut *store, (16,)).await? as u32;
    let arr = malloc.call_async(&mut *store, (8,)).await? as u32;
    let d = store.data().mem.data();
    for (i, b) in b"/content/.keep\0".iter().enumerate() {
        unsafe { *d[p as usize + i].get() = *b };
    }
    unsafe { *(d[arr as usize].get() as *mut u32) = p };
    unsafe { *(d[arr as usize + 4].get() as *mut u32) = 0 };
    Ok(arr)
}

/// What WasmPlayer.js does before createWebClient().
async fn prepare(store: &mut Store<St>, inst: &Instance) -> Result<()> {
    let a = str_arg(store, inst, "desktop").await?;
    binds::call(store, inst, "set_pretend_platform", &[a]).await?;
    let a = str_arg(store, inst, WEB_BASE).await?;
    binds::call(store, inst, "set_roblox_base_url", &[a]).await?;
    let a = str_arg(store, inst, "").await?;
    binds::call(store, inst, "set_channel", &[a]).await?;
    let a = str_arg(store, inst, "").await?;
    binds::call(store, inst, "set_browser_tracker_id", &[a]).await?;
    let a = str_arg(store, inst, "en-us").await?;
    let b = str_arg(store, inst, "en-us").await?;
    binds::call(store, inst, "set_account_locales", &[a, b]).await?;

    let flags = br#"{"applicationSettings":{"BaseUrl":"https://www.roblox.com/","FIntWasmMemoryPages":"32768"},"RobloxLocaleId":"en-us","GameLocaleId":"en-us"}"#;
    let fp = binds::guest_string_bytes(store, inst, flags).await?;
    binds::call(store, inst, "set_preloaded_flags", &[imports::embind::Arg::I32(fp)]).await?;

    let place: i64 = std::env::var("WC_PLACE").ok().and_then(|v| v.parse().ok()).unwrap_or(1818);
    binds::call(store, inst, "set_place_id", &[imports::embind::Arg::I64(place as u64)]).await?;

    // VFS bootstrap: the metadata tree, then the patch model the loader expects
    // to find already present.
    let fs_base = std::env::var("WC_CONTENT").unwrap_or_else(|_| "data/content".into());
    let reg = inst.get_typed_func::<(i32, i32), i32>(&mut *store, "rbx_fs_register_asset")?;
    let mut n = 0;
    let meta = std::fs::read_to_string("data/meta.json").unwrap_or_default();
    for ent in meta.split('{').skip(1) {
        let path = field(ent, "path");
        let size: u32 = field(ent, "size").parse().unwrap_or(1);
        if path.is_empty() {
            continue;
        }
        let p = import_bytes(store, inst, path.as_bytes()).await?;
        reg.call_async(&mut *store, (p as i32, size as i32)).await?;
        n += 1;
    }
    eprintln!("[fs  ] registered {n} asset paths under {fs_base}");

    let pre = inst.get_typed_func::<(i32, i32, i32), i32>(&mut *store, "rbx_fs_preload_asset")?;
    for rel in ["/models/InExperience/InExperience.rbxm",
                "/models/InExperience/InExperience_checksum"] {
        let disk = format!("{fs_base}{rel}");
        let Ok(body) = std::fs::read(&disk) else { continue };
        let p = import_bytes(store, inst, rel.as_bytes()).await?;
        let d = import_bytes(store, inst, &body).await?;
        let rc = pre.call_async(&mut *store, (p as i32, d as i32, body.len() as i32)).await?;
        eprintln!("[fs  ] preload {rel} ({} bytes) -> {rc}", body.len());
    }

    let app = import_bytes(store, inst, br#"{"WebLogin":"2","RobloxLocaleId":"en-us","GameLocaleId":"en-us"}"#).await?;
    let add = inst.get_typed_func::<(i32, i32, i32), i32>(&mut *store, "rbx_fs_add_file")?;
    let path = import_bytes(store, inst, b"/appData/LocalStorage/appStorage.json").await?;
    add.call_async(&mut *store, (path as i32, app as i32, 61)).await?;

    let sb = inst.get_typed_func::<(i32,), ()>(&mut *store, "rbx_fs_set_base_url")?;
    let bp = import_bytes(store, inst, ASSET_BASE.as_bytes()).await?;
    sb.call_async(&mut *store, (bp as i32,)).await?;

    eprintln!("[prep] place={place} canvas={CANVAS_W}x{CANVAS_H} assets={} preload={} files={}", store.data().assets.len(), store.data().preload.len(), store.data().files.len());
    Ok(())
}

fn field(ent: &str, name: &str) -> String {
    let key = format!("\"{name}\":");
    let Some(i) = ent.find(&key) else { return String::new() };
    let rest = &ent[i + key.len()..];
    let rest = rest.trim_start();
    if let Some(r) = rest.strip_prefix('"') {
        let Some(j) = r.find('"') else { return String::new() };
        return r[..j].to_string();
    }
    let end = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
    rest[..end].to_string()
}

async fn str_arg(store: &mut Store<St>, inst: &Instance, s: &str) -> Result<imports::embind::Arg> {
    Ok(imports::embind::Arg::I32(binds::guest_string(store, inst, s).await?))
}

// small alias so the flags call reads clearly
/* arg shorthand */

/// Allocate guest memory and copy `b` plus a NUL (engine strings).
async fn import_bytes(store: &mut Store<St>, inst: &Instance, b: &[u8]) -> Result<u32> {
    let malloc = inst.get_typed_func::<(i32,), i32>(&mut *store, "malloc")?;
    let p = malloc.call_async(&mut *store, (b.len() as i32 + 1,)).await? as u32;
    if p == 0 {
        return Err(anyhow!("guest malloc failed"));
    }
    let d = store.data().mem.data();
    for (i, x) in b.iter().enumerate() {
        unsafe { *d[p as usize + i].get() = *x };
    }
    unsafe { *d[p as usize + b.len()].get() = 0 };
    Ok(p)
}
