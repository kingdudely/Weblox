//! Per-instance state and guest-memory helpers.
use std::collections::HashMap;

use wasmtime::{Caller, Instance, SharedMemory};

pub const CANVAS_W: u32 = 1280;
pub const CANVAS_H: u32 = 720;

pub struct St {
    pub mem: SharedMemory,
    pub inst: Option<Instance>,
    pub engine: Option<wasmtime::Engine>,
    pub module: Option<wasmtime::Module>,
    pub base: String,
    pub place: i64,
    /// rbx_fs_register_asset: path -> declared size
    pub assets: HashMap<String, u32>,
    /// rbx_fs_add_file: path -> bytes
    pub files: HashMap<String, Vec<u8>>,
    /// rbx_fs_preload_asset: path -> bytes
    pub preload: HashMap<String, Vec<u8>>,
    pub binds: std::collections::HashMap<String, (u32, u32, String)>,
    pub exited: bool,
    pub is_main: bool,
    pub gm_calls: u32,
    pub main_ptr: Option<u32>,
    pub rt: Option<crate::fibre::Rt>,
    pub cur: u32,
    pub in_drain: bool,
    pub mbx_task: bool,
    pub mk_calls: u32,
    pub turn_depth: u32,
    /// embind type id -> kind, from _embind_register_*
    pub types: HashMap<u32, String>,
}

impl St {
    pub fn new(mem: SharedMemory) -> Self {
        St {
            mem,
            inst: None,
            engine: None,
            module: None,
            base: String::new(),
            place: 0,
            assets: HashMap::new(),
            files: HashMap::new(),
            preload: HashMap::new(),
            binds: Default::default(),
            exited: false,
            is_main: false,
            gm_calls: 0,
            main_ptr: None,
            rt: None,
            cur: 0,
            in_drain: false,
            mbx_task: false,
            mk_calls: 0,
            turn_depth: 0,
            types: Default::default(),
        }
    }
}

/// Read a NUL-terminated guest string.
pub fn rstr(c: &Caller<'_, St>, p: u32) -> String {
    if p == 0 {
        return String::new();
    }
    let d = c.data().mem.data();
    let mut out = Vec::new();
    for i in p as usize.. {
        if i >= d.len() {
            break;
        }
        let b = unsafe { *d[i].get() };
        if b == 0 {
            break;
        }
        out.push(b);
    }
    String::from_utf8_lossy(&out).into_owned()
}

pub fn rbytes(c: &Caller<'_, St>, p: u32, len: u32) -> Vec<u8> {
    let d = c.data().mem.data();
    (0..len as usize)
        .map(|i| unsafe { *d[p as usize + i].get() })
        .collect()
}

pub fn wbytes(c: &mut Caller<'_, St>, p: u32, src: &[u8]) {
    let d = c.data().mem.data();
    for (i, b) in src.iter().enumerate() {
        unsafe { *d[p as usize + i].get() = *b };
    }
}

/// Write a NUL-terminated string, truncating to `cap`.
pub fn wstr(c: &mut Caller<'_, St>, p: u32, s: &str, cap: u32) {
    if cap == 0 {
        return;
    }
    let n = s.len().min(cap as usize - 1);
    wbytes(c, p, &s.as_bytes()[..n]);
    unsafe { *c.data().mem.data()[p as usize + n].get() = 0 };
}
