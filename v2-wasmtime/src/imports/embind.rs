//! embind registration: the engine registers its binding table at startup and
//! the host calls entries back through it (mirrors impl/embind.c).
use std::collections::HashMap;

use wasmtime::{Caller, Linker};

use crate::state::{rstr, St};

#[derive(Clone, Copy, PartialEq)]
pub enum Arg {
    I32(u32),
    I64(u64),
    F32(f32),
    F64(f64),
}

/// Argument kinds come from the binding signature string (`v` receiver, then
/// one char per argument), exactly as WebClient.js encodes them.
pub fn kind(sig: &str, k: usize) -> Option<char> {
    sig.as_bytes().get(k + 2).map(|b| *b as char)
}

pub fn ret_is_void(sig: &str) -> bool {
    sig.starts_with('v')
}

/// Embind registrations are process wide: a worker store never sees the
/// initialiser pass that creates them.
pub fn binds() -> &'static std::sync::Mutex<std::collections::HashMap<String, (u32, u32, String)>> {
    static B: std::sync::OnceLock<std::sync::Mutex<std::collections::HashMap<String, (u32, u32, String)>>> =
        std::sync::OnceLock::new();
    B.get_or_init(Default::default)
}

pub fn register(l: &mut Linker<St>) -> anyhow::Result<()> {
    // (name, argcount, raw arg types, invoker, signature)
    l.func_wrap("env", "_embind_register_function",
        |mut c: Caller<'_, St>, name: i32, _argc: i32, _types: i32, sig: i32,
         invoker: i32, _id: i32, _a: i32, _b: i32| {
            let n = rstr(&c, name as u32);
            let s = rstr(&c, sig as u32);
            crate::trace::hit("_embind_register_function");
            eprintln!("[bind] {n} sig={s} invoker={invoker}");
            let _ = &c;
            binds().lock().unwrap().insert(n, (invoker as u32, _id as u32, s));
        })?;

    // type registry: ids are what the engine uses for its own type checks
    l.func_wrap("env", "_embind_register_void", |mut c: Caller<'_, St>, id: i32, _n: i32| {
        c.data_mut().types.insert(id as u32, "void".into());
    })?;
    l.func_wrap("env", "_embind_register_bool",
        |mut c: Caller<'_, St>, id: i32, _n: i32, _s: i32, _t: i32| {
            c.data_mut().types.insert(id as u32, "bool".into());
        })?;
    l.func_wrap("env", "_embind_register_integer",
        |mut c: Caller<'_, St>, id: i32, _n: i32, size: i32, _lo: i32, _hi: i32| {
            let k = if size == 8 { "i64" } else { "i32" };
            c.data_mut().types.insert(id as u32, k.into());
        })?;
    l.func_wrap("env", "_embind_register_bigint",
        |mut c: Caller<'_, St>, id: i32, _n: i32, _s: i32, _lo: i64, _hi: i64| {
            c.data_mut().types.insert(id as u32, "i64".into());
        })?;
    l.func_wrap("env", "_embind_register_float",
        |mut c: Caller<'_, St>, id: i32, _n: i32, size: i32| {
            let k = if size == 8 { "f64" } else { "f32" };
            c.data_mut().types.insert(id as u32, k.into());
        })?;
    l.func_wrap("env", "_embind_register_std_string",
        |mut c: Caller<'_, St>, id: i32, _n: i32| {
            c.data_mut().types.insert(id as u32, "str".into());
        })?;
    l.func_wrap("env", "_embind_register_std_wstring",
        |mut c: Caller<'_, St>, id: i32, _n: i32, _s: i32| {
            c.data_mut().types.insert(id as u32, "str".into());
        })?;
    l.func_wrap("env", "_embind_register_emval", |mut c: Caller<'_, St>, id: i32| {
        c.data_mut().types.insert(id as u32, "emval".into());
    })?;
    l.func_wrap("env", "_embind_register_memory_view",
        |mut c: Caller<'_, St>, id: i32, _kind: i32, n: i32| {
            crate::trace::hit("mem_view");
            eprintln!("[mv ] id={id} name={:?}", rstr(&c, n as u32));
            c.data_mut().types.insert(id as u32, "ptr".into());
        })?;
    Ok(())
}

pub type Binds = HashMap<String, (u32, String)>;
