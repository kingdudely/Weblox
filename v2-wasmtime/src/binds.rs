//! Calling the engine's own embind bindings (set_place_id, ...).
use anyhow::{anyhow, Result};
use wasmtime::{Instance, Store, Val};

use crate::imports::embind::{kind, ret_is_void, Arg};
use crate::state::St;

pub async fn call(store: &mut Store<St>, inst: &Instance, name: &str, args: &[Arg]) -> Result<Option<Val>> {
    let (invoker, id, sig) = store
        .data()
        .binds
        .get(name)
        .cloned()
        .ok_or_else(|| anyhow!("binding {name} not registered"))?;
    eprintln!("[bind] call {name} id={id} invoker={invoker} sig={sig} args={}", args.len());

    // The invoker thunk is `(u32 binding_id, args...)`, the arguments typed by
    // the binding signature (argument k is sig[k + 2]).
    let mut vals = vec![Val::I32(id as i32)];
    for (k, a) in args.iter().enumerate() {
        let v = match (kind(&sig, k), a) {
            (Some('j'), Arg::I64(x)) => Val::I64(*x as i64),
            (Some('f'), _) => Val::F32(match a {
                Arg::F32(x) => x.to_bits(),
                Arg::I32(x) => *x,
                _ => 0,
            }),
            (Some('d'), Arg::F64(x)) => Val::F64(x.to_bits()),
            (_, Arg::I32(x)) => Val::I32(*x as i32),
            (_, Arg::I64(x)) => Val::I64(*x as i64),
            (_, Arg::F32(x)) => Val::F32(x.to_bits()),
            (_, Arg::F64(x)) => Val::F64(x.to_bits()),
        };
        vals.push(v);
    }

    let table = inst
        .get_table(&mut *store, "__indirect_function_table")
        .ok_or_else(|| anyhow!("no table"))?;
    let slot = table
        .get(&mut *store, invoker as u64)
        .ok_or_else(|| anyhow!("binding {name}: invoker {invoker} missing"))?;
    let f = match slot {
        wasmtime::Ref::Func(Some(f)) => f,
        _ => return Err(anyhow!("binding {name}: invoker {invoker} is not a func")),
    };

    f.call(&mut *store, &vals, &mut [])?;
    let _ = sig;
    Ok(None)
}

/// Same wire form, from a byte slice.
pub async fn guest_string_bytes(store: &mut Store<St>, inst: &Instance, b: &[u8]) -> Result<u32> {
    let malloc = inst.get_typed_func::<(i32,), i32>(&mut *store, "malloc")?;
    let n = b.len() as i32 + 5;
    let p = malloc.call(&mut *store, (n,))? as u32;
    if p == 0 {
        return Err(anyhow!("guest malloc failed"));
    }
    let d = store.data().mem.data();
    unsafe { *(d[p as usize].get() as *mut u32) = b.len() as u32 };
    for (i, x) in b.iter().enumerate() {
        unsafe { *d[p as usize + 4 + i].get() = *x };
    }
    unsafe { *d[p as usize + 4 + b.len()].get() = 0 };
    Ok(p)
}

/// Copy a string into guest memory as embind's std::string wire form
/// (u32 length followed by the bytes) and return its address.
pub async fn guest_string(store: &mut Store<St>, inst: &Instance, s: &str) -> Result<u32> {
    let malloc = inst.get_typed_func::<(i32,), i32>(&mut *store, "malloc")?;
    let n = s.len() as i32 + 4;
    let p = malloc.call(&mut *store, (n,))? as u32;
    if p == 0 {
        return Err(anyhow!("guest malloc failed"));
    }
    let d = store.data().mem.data();
    unsafe { *(d[p as usize].get() as *mut u32) = s.len() as u32 };
    for (i, b) in s.as_bytes().iter().enumerate() {
        unsafe { *d[p as usize + 4 + i].get() = *b };
    }
    Ok(p)
}
