//! The engine's OPFS cache, on a directory.
//!
//! The page backs these calls with `navigator.storage`; here they are plain
//! files under `data/opfs`.  Anything that cannot be served (the synchronous
//! fast path) reports failure so the engine retries through the async calls.
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::PathBuf;

use wasmtime::{Caller, Linker};

use crate::state::{rbytes, rstr, wbytes, St};

fn root() -> PathBuf {
    PathBuf::from(std::env::var("WC_OPFS").unwrap_or_else(|_| "data/opfs".into()))
}

fn path(name: &str) -> Option<PathBuf> {
    let n = name.trim_start_matches('/');
    if n.is_empty() || n.contains("..") {
        return None;
    }
    Some(root().join(n))
}

fn read(name: &str) -> Option<Vec<u8>> {
    let p = path(name)?;
    std::fs::read(p).ok()
}

fn write(name: &str, body: &[u8]) -> bool {
    let Some(p) = path(name) else { return false };
    if let Some(d) = p.parent() {
        let _ = std::fs::create_dir_all(d);
    }
    std::fs::write(p, body).is_ok()
}

fn wbytes_st(mem: &wasmtime::SharedMemory, p: u32, src: &[u8]) {
    let d = mem.data();
    unsafe { std::ptr::copy_nonoverlapping(src.as_ptr(), d[p as usize].get(), src.len()) };
}

pub fn register(l: &mut Linker<St>) -> wasmtime::Result<()> {
    l.func_wrap("env", "__asyncjs__rbx_opfs_clear", || -> i32 {
        crate::trace::hit("opfs_clear");
        let _ = std::fs::remove_dir_all(root());
        let _ = std::fs::create_dir_all(root());
        1
    })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_contains", |c: Caller<'_, St>, p: i32| -> i32 {
        crate::trace::hit("opfs_contains");
        read(&rstr(&c, p as u32)).is_some() as i32
    })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_file_size", |c: Caller<'_, St>, p: i32| -> i32 {
        crate::trace::hit("opfs_file_size");
        read(&rstr(&c, p as u32)).map(|b| b.len() as i32).unwrap_or(0)
    })?;
    l.func_wrap_async("env", "__asyncjs__rbx_opfs_get", |c: Caller<'_, St>, (p, len): (i32, i32)| {
        crate::trace::hit("opfs_get");
        let body = read(&rstr(&c, p as u32));
        let sp = c.data().rt.as_ref().expect("rt").store;
        let fut: Box<dyn std::future::Future<Output = i32> + Send> = Box::new(async move {
            let st = sp.get();
            let mem = st.data().mem.clone();
            wbytes_st(&mem, len as u32, &0u32.to_le_bytes());
            let Some(body) = body else { return 0 };
            if body.is_empty() { return 0 }
            let Some(inst) = st.data().inst else { return 0 };
            let Ok(m) = inst.get_typed_func::<(i32,), i32>(&mut *st, "malloc") else { return 0 };
            let d = m.call_async(&mut *st, (body.len() as i32,)).await.unwrap_or(0) as u32;
            if d == 0 { return 0 }
            wbytes_st(&mem, d, &body);
            wbytes_st(&mem, len as u32, &(body.len() as u32).to_le_bytes());
            d as i32
        });
        fut
    })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_get_buffer",
        |mut c: Caller<'_, St>, p: i32, dst: i32, n: i32| -> i32 {
            crate::trace::hit("opfs_get_buffer");
            let Some(body) = read(&rstr(&c, p as u32)) else { return 0 };
            let n = (n.max(0) as usize).min(body.len());
            wbytes(&mut c, dst as u32, &body[..n]);
            n as i32
        })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_get_buffer_offset",
        |mut c: Caller<'_, St>, p: i32, dst: i32, n: i32, off: i32, limit: i32| -> i32 {
            crate::trace::hit("opfs_get_buffer_offset");
            let Some(body) = read(&rstr(&c, p as u32)) else { return 0 };
            let want = (limit.max(0) as usize).min(n.max(0) as usize);
            let off = off.max(0) as usize;
            if off >= body.len() || want == 0 {
                return 0;
            }
            let end = (off + want).min(body.len());
            wbytes(&mut c, dst as u32, &body[off..end]);
            (end - off) as i32
        })?;
    l.func_wrap_async("env", "__asyncjs__rbx_opfs_get_offset",
        |c: Caller<'_, St>, (p, len, off, n): (i32, i32, i32, i32)| {
            crate::trace::hit("opfs_get_offset");
            let body = read(&rstr(&c, p as u32));
            let sp = c.data().rt.as_ref().expect("rt").store;
            let fut: Box<dyn std::future::Future<Output = i32> + Send> = Box::new(async move {
                let st = sp.get();
                let mem = st.data().mem.clone();
                wbytes_st(&mem, len as u32, &0u32.to_le_bytes());
                let Some(body) = body else { return 0 };
                let off = off.max(0) as usize;
                if off >= body.len() { return 0 }
                let end = (off + n.max(0) as usize).min(body.len());
                let slice = body[off..end].to_vec();
                let Some(inst) = st.data().inst else { return 0 };
                let Ok(m) = inst.get_typed_func::<(i32,), i32>(&mut *st, "malloc") else { return 0 };
                let d = m.call_async(&mut *st, (slice.len() as i32,)).await.unwrap_or(0) as u32;
                if d == 0 { return 0 }
                wbytes_st(&mem, d, &slice);
                wbytes_st(&mem, len as u32, &(slice.len() as u32).to_le_bytes());
                d as i32
            });
            fut
        })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_list", |mut c: Caller<'_, St>, total: i32, count: i32| -> i32 {
        crate::trace::hit("opfs_list");
        wbytes(&mut c, total as u32, &0u32.to_le_bytes());
        wbytes(&mut c, count as u32, &0u32.to_le_bytes());
        let mut names = Vec::new();
        let mut bytes = 0u64;
        if let Ok(rd) = std::fs::read_dir(root()) {
            for e in rd.flatten() {
                let Ok(md) = e.metadata() else { continue };
                if !md.is_file() || md.len() == 0 {
                    continue;
                }
                bytes += md.len();
                names.push(e.file_name().to_string_lossy().into_owned());
            }
        }
        if names.is_empty() {
            return 0;
        }
        let joined = format!("{}\0", names.join("\n"));
        let Some(inst) = c.data().inst else { return 0 };
        let Ok(m) = inst.get_typed_func::<(i32,), i32>(&mut c, "malloc") else { return 0 };
        let d = crate::fibre::run_sync(m.call_async(&mut c, (joined.len() as i32,))).unwrap_or(0) as u32;
        if d == 0 {
            return 0;
        }
        wbytes(&mut c, d, joined.as_bytes());
        wbytes(&mut c, total as u32, &(bytes as u32).to_le_bytes());
        wbytes(&mut c, count as u32, &(names.len() as u32).to_le_bytes());
        d as i32
    })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_list_sized", |mut c: Caller<'_, St>, total: i32, count: i32| -> i32 {
        crate::trace::hit("opfs_list_sized");
        wbytes(&mut c, total as u32, &0u32.to_le_bytes());
        wbytes(&mut c, count as u32, &0u32.to_le_bytes());
        0
    })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_open", |c: Caller<'_, St>, _p: i32, _w: i32| {
        crate::trace::hit("opfs_open");
        let _ = &c;
    })?;
    l.func_wrap("env", "rbx_opfs_close", |c: Caller<'_, St>, _p: i32| {
        crate::trace::hit("opfs_close");
        let _ = &c;
    })?;
    l.func_wrap("env", "rbx_opfs_read_sync_if_cached",
        |mut c: Caller<'_, St>, p: i32, dst: i32, off: i32, n: i32| -> i32 {
            crate::trace::hit("opfs_read_sync");
            let Some(body) = read(&rstr(&c, p as u32)) else { return -1 };
            let off = off.max(0) as usize;
            if off >= body.len() {
                return 0;
            }
            let end = (off + n.max(0) as usize).min(body.len());
            wbytes(&mut c, dst as u32, &body[off..end]);
            (end - off) as i32
        })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_set", |c: Caller<'_, St>, p: i32, data: i32, n: i32| -> i32 {
        crate::trace::hit("opfs_set");
        if n <= 0 {
            return 0;
        }
        let body = rbytes(&c, data as u32, n as u32);
        write(&rstr(&c, p as u32), &body) as i32
    })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_remove", |c: Caller<'_, St>, p: i32| -> i32 {
        crate::trace::hit("opfs_remove");
        match path(&rstr(&c, p as u32)) {
            Some(p) => std::fs::remove_file(p).is_ok() as i32,
            None => 0,
        }
    })?;
    l.func_wrap("env", "__asyncjs__rbx_opfs_remove_batch", |c: Caller<'_, St>, list: i32| -> i32 {
        crate::trace::hit("opfs_remove_batch");
        let mut n = 0;
        for name in rstr(&c, list as u32).split('\n') {
            if name.is_empty() {
                continue;
            }
            if let Some(p) = path(name) {
                if std::fs::remove_file(p).is_ok() {
                    n += 1;
                }
            }
        }
        n
    })?;
    l.func_wrap("env", "rbx_opfs_free", |c: Caller<'_, St>, p: i32| {
        crate::trace::hit("opfs_free");
        let sp = c.data().rt.as_ref().expect("rt").store;
        crate::fibre::spawn_simple(async move {
            let st = sp.get();
            let Some(inst) = st.data().inst else { return };
            if let Ok(f) = inst.get_typed_func::<(i32,), ()>(&mut *st, "free") {
                let _ = f.call_async(&mut *st, (p,)).await;
            }
        });
    })?;
    l.func_wrap("env", "rbx_opfs_install_helpers", |c: Caller<'_, St>, _a: i32, _b: i32| {
        crate::trace::hit("opfs_install");
        let _ = &c;
    })?;
    Ok(())
}

/// Keep the imports used only for side effects readable.
#[allow(dead_code)]
fn _unused(mut f: std::fs::File) {
    let _ = f.seek(SeekFrom::Start(0));
    let mut b = Vec::new();
    let _ = f.read_to_end(&mut b);
    let _ = f.write_all(&b);
}
