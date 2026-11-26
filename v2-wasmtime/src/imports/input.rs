//! Real input from the X11 window, delivered through the engine's callbacks.
//!
//! The engine registers emscripten callback functions per event kind; nodes are
//! queued by `gfx` and handed over here at the next safe point, with the same
//! event structs emscripten's html5.c would fill.
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use wasmtime::{Caller, Linker};

use crate::state::St;

pub const KEYPRESS: u32 = 1;
pub const KEYDOWN: u32 = 2;
pub const KEYUP: u32 = 3;
pub const MOUSEDOWN: u32 = 7;
pub const MOUSEUP: u32 = 8;
pub const MOUSEMOVE: u32 = 9;
pub const MOUSELEAVE: u32 = 12;
pub const MOUSEENTER: u32 = 13;
pub const WHEEL: u32 = 14;
pub const TOUCHSTART: u32 = 15;
pub const TOUCHEND: u32 = 16;
pub const TOUCHMOVE: u32 = 17;
pub const TOUCHCANCEL: u32 = 18;
pub const FOCUS: u32 = 19;
pub const BLUR: u32 = 20;

// Emscripten DOM event ids used as callback slots; 26/27 gamepad, 38 pointerlockerror.
pub const GAMEPADCONNECTED: u32 = 26;
pub const GAMEPADDISCONNECTED: u32 = 27;
pub const POINTERLOCKERROR: u32 = 38;

const N: usize = 39;
static CB: [AtomicU32; N] = [const { AtomicU32::new(0) }; N];
static USER: [AtomicU32; N] = [const { AtomicU32::new(0) }; N];

#[derive(Clone)]
pub enum Ev {
    Key { down: bool, key: String, code: String, keycode: u32, ch: u32, modifiers: u32, repeat: bool },
    Mouse { down: Option<u16>, x: f64, y: f64, mx: f64, my: f64, button: u16, modifiers: u32 },
    Wheel { x: f64, y: f64, dx: f64, dy: f64, mode: u32, modifiers: u32 },
    Touch { phase: u32, x: f64, y: f64, id: u32, modifiers: u32 },
    Focus { on: bool },
}

static Q: Mutex<Vec<Ev>> = Mutex::new(Vec::new());

pub fn push(e: Ev) {
    let mut q = Q.lock().unwrap();
    if q.len() < 512 { q.push(e) }
}

pub fn pending() -> bool {
    !Q.lock().unwrap().is_empty()
}

fn cb(id: u32) -> (u32, u32) {
    (CB[id as usize].load(Ordering::Relaxed), USER[id as usize].load(Ordering::Relaxed))
}

fn w32(c: &mut wasmtime::Store<St>, p: u32, v: u32) {
    let d = c.data().mem.data();
    unsafe { *(d[p as usize].get() as *mut u32) = v }
}

fn wstr(c: &mut wasmtime::Store<St>, p: u32, s: &str, cap: usize) {
    let d = c.data().mem.data();
    let n = s.len().min(cap - 1);
    for (i, b) in s.as_bytes().iter().take(n).enumerate() {
        unsafe { *d[p as usize + i].get() = *b }
    }
    unsafe { *d[p as usize + n].get() = 0 }
}

fn emit(c: &mut wasmtime::Store<St>, id: u32, ev: &Ev) {
    let (func, user) = cb(id);
    if func == 0 { return }
    let inst = match c.data().inst { Some(i) => i, None => return };
    let stack = 0x3000_0000u32 + (id * 0x10000);
    let d = c.data().mem.data();
    unsafe { std::ptr::write_bytes(d[stack as usize].get(), 0, 0x10000) };
    w32(c, stack, 0);
    let (ct, st, at, mt) = modifiers(ev);
    match ev {
        Ev::Key { key, code, keycode, ch, repeat, down, .. } => {
            wstr(c, stack + 32, key, 32);
            wstr(c, stack + 64, code, 32);
            w32(c, stack + 96, *keycode);
            w32(c, stack + 100, *keycode);
            wstr(c, stack + 104, "none", 32);
            w32(c, stack + 112, ct);
            w32(c, stack + 116, st);
            w32(c, stack + 120, at);
            w32(c, stack + 124, mt);
            w32(c, stack + 128, *repeat as u32);
            w32(c, stack + 132, 0);
            w32(c, stack + 136, *ch);
            let evid = if *down { KEYDOWN } else { KEYUP };
            let _ = evid;
        }
        Ev::Mouse { down, x, y, mx, my, button, .. } => {
            w32(c, stack, 1);
            w32(c, stack + 16, (*x * 96.0) as i32 as u32);
            w32(c, stack + 20, (*y * 96.0) as i32 as u32);
            w32(c, stack + 24, *x as i32 as u32);
            w32(c, stack + 28, *y as i32 as u32);
            w32(c, stack + 32, *mx as i32 as u32);
            w32(c, stack + 36, *my as i32 as u32);
            w32(c, stack + 40, ct);
            w32(c, stack + 44, st);
            w32(c, stack + 48, at);
            w32(c, stack + 52, mt);
            let b = down.unwrap_or(*button);
            let _ = b;
            w32(c, stack + 56, *button as u32);
            w32(c, stack + 58, 0xffff);
        }
        Ev::Wheel { x, y, dx, dy, mode, .. } => {
            w32(c, stack, 1);
            w32(c, stack + 16, (*x * 96.0) as i32 as u32);
            w32(c, stack + 20, (*y * 96.0) as i32 as u32);
            w32(c, stack + 24, *x as i32 as u32);
            w32(c, stack + 28, *y as i32 as u32);
            let dd = c.data().mem.data();
            unsafe { *(dd[stack as usize + 64].get() as *mut f64) = *dx };
            unsafe { *(dd[stack as usize + 72].get() as *mut f64) = *dy };
            unsafe { *(dd[stack as usize + 80].get() as *mut f64) = 0.0 };
            w32(c, stack + 88, *mode);
        }
        Ev::Touch { phase, x, y, id: tid, .. } => {
            let _ = phase;
            w32(c, stack, 1);
            w32(c, stack + 36, 1);
            w32(c, stack + 40, *tid);
            w32(c, stack + 44, 1);
            w32(c, stack + 48, *x as i32 as u32);
            w32(c, stack + 52, *y as i32 as u32);
            w32(c, stack + 56, *x as i32 as u32);
            w32(c, stack + 60, *y as i32 as u32);
            w32(c, stack + 64, 1);
            w32(c, stack + 68, 1);
        }
        Ev::Focus { .. } => {}
    }
    let Some(f) = inst.get_table(&mut *c, "__indirect_function_table")
        .and_then(|t| match t.get(&mut *c, func as u64) { Some(wasmtime::Ref::Func(Some(f))) => Some(f), _ => None })
    else { return };
    let evid = match ev {
        Ev::Key { down, .. } => if *down { KEYDOWN } else { KEYUP },
        Ev::Mouse { down, .. } => down.map(|b| if b == 0 { MOUSEMOVE } else { MOUSEDOWN }).unwrap_or(MOUSEUP),
        Ev::Wheel { .. } => WHEEL,
        Ev::Touch { phase, .. } => *phase,
        Ev::Focus { on } => if *on { FOCUS } else { BLUR },
    };
    let mut out = [wasmtime::Val::I32(0)];
    let _ = crate::fibre::run_sync(f.call_async(&mut *c, &[
        wasmtime::Val::I32(evid as i32),
        wasmtime::Val::I32(stack as i32),
        wasmtime::Val::I32(user as i32),
    ], &mut out));
}

fn modifiers(ev: &Ev) -> (u32, u32, u32, u32) {
    let m = match ev {
        Ev::Key { modifiers, .. } | Ev::Mouse { modifiers, .. } | Ev::Wheel { modifiers, .. } | Ev::Touch { modifiers, .. } => *modifiers,
        _ => 0,
    };
    (m & 4, (m >> 1) & 1, m & 1, (m >> 2) & 1)
}

pub fn pump(c: &mut wasmtime::Store<St>) {
    if !pending() { return }
    let evs: Vec<Ev> = { let mut q = Q.lock().unwrap(); std::mem::take(&mut *q) };
    for e in evs {
        match &e {
            Ev::Key { down, .. } => {
                emit(c, if *down { KEYDOWN } else { KEYUP }, &e);
                if *down { emit(c, KEYPRESS, &e) }
            }
            Ev::Mouse { down, .. } => {
                emit(c, if down.is_some() { MOUSEDOWN } else { MOUSEMOVE }, &e);
                if down.is_some() { emit(c, MOUSEUP, &e) }
            }
            Ev::Wheel { .. } => emit(c, WHEEL, &e),
            Ev::Touch { phase, .. } => emit(c, *phase, &e),
            Ev::Focus { on } => emit(c, if *on { FOCUS } else { BLUR }, &e),
        }
    }
}

pub fn register(l: &mut Linker<St>) -> wasmtime::Result<()> {
    macro_rules! set {
        ($name:literal, $id:expr) => {
            l.func_wrap("env", $name,
                |_a: i32, func: i32, user: i32, _c: i32, _t: i32| -> i32 {
                    CB[$id as usize].store(func as u32, Ordering::Relaxed);
                    USER[$id as usize].store(user as u32, Ordering::Relaxed);
                    0
                })?;
        };
    }
    set!("emscripten_set_keydown_callback_on_thread", KEYDOWN);
    set!("emscripten_set_keyup_callback_on_thread", KEYUP);
    set!("emscripten_set_mousedown_callback_on_thread", MOUSEDOWN);
    set!("emscripten_set_mouseup_callback_on_thread", MOUSEUP);
    set!("emscripten_set_mousemove_callback_on_thread", MOUSEMOVE);
    set!("emscripten_set_wheel_callback_on_thread", WHEEL);
    set!("emscripten_set_touchstart_callback_on_thread", TOUCHSTART);
    set!("emscripten_set_touchend_callback_on_thread", TOUCHEND);
    set!("emscripten_set_touchmove_callback_on_thread", TOUCHMOVE);
    set!("emscripten_set_touchcancel_callback_on_thread", TOUCHCANCEL);
    set!("emscripten_set_mouseleave_callback_on_thread", MOUSELEAVE);
    set!("emscripten_set_mouseenter_callback_on_thread", MOUSEENTER);
    set!("emscripten_set_blur_callback_on_thread", BLUR);
    set!("emscripten_set_focus_callback_on_thread", FOCUS);
    // JS `Kt(e,r,t,n,a)`: target=1/2 (document/window), store callback+user.
    l.func_wrap("env", "emscripten_set_pointerlockerror_callback_on_thread",
        |target: i32, user: i32, _capture: i32, func: i32, _thread: i32| -> i32 {
            if crate::gfx::handles().is_none() { return -1 }
            if target != 1 && target != 2 { return -4 }
            CB[POINTERLOCKERROR as usize].store(func as u32, Ordering::Relaxed);
            USER[POINTERLOCKERROR as usize].store(user as u32, Ordering::Relaxed);
            0
        })?;
    // JS `Lt(e,r)`: -4 bad target, -1 unsupported, -2 no activation, 1 deferred, 0 success.
    l.func_wrap("env", "emscripten_request_pointerlock", |target: i32, deferred: i32| -> i32 {
        if target != 1 && target != 2 { return -4 }
        if crate::gfx::handles().is_none() { return -1 }
        if deferred != 0 { return 1 }
        crate::gfx::pointer_lock(true);
        0
    })?;
    l.func_wrap("env", "emscripten_exit_pointerlock", || -> i32 {
        if crate::gfx::handles().is_none() { return -1 }
        crate::gfx::pointer_lock(false);
        0
    })?;
    // No gamepads exist on X11: `Mt` -> 0, `rr` -> 0, `Ft` -> -5 for any index.
    l.func_wrap("env", "emscripten_get_num_gamepads", || -> i32 { 0 })?;
    l.func_wrap("env", "emscripten_sample_gamepad_data", || -> i32 { 0 })?;
    l.func_wrap("env", "emscripten_get_gamepad_status", |_a: i32, _b: i32| -> i32 { -5 })?;
    l.func_wrap("env", "emscripten_set_gamepadconnected_callback_on_thread",
        |user: i32, _capture: i32, func: i32, _thread: i32| -> i32 {
            CB[GAMEPADCONNECTED as usize].store(func as u32, Ordering::Relaxed);
            USER[GAMEPADCONNECTED as usize].store(user as u32, Ordering::Relaxed);
            0
        })?;
    l.func_wrap("env", "emscripten_set_gamepaddisconnected_callback_on_thread",
        |user: i32, _capture: i32, func: i32, _thread: i32| -> i32 {
            CB[GAMEPADDISCONNECTED as usize].store(func as u32, Ordering::Relaxed);
            USER[GAMEPADDISCONNECTED as usize].store(user as u32, Ordering::Relaxed);
            0
        })?;
    l.func_wrap("env", "js_request_keyboard_layout", |_c: Caller<'_, St>| { })?;
    l.func_wrap("env", "js_keyboard_layout_status", |_c: Caller<'_, St>| -> i32 { 1 })?;
    l.func_wrap("env", "js_copy_keyboard_layout_key",
        |mut c: Caller<'_, St>, code: i32, dst: i32, cap: i32| {
            let s = crate::state::rstr(&c, code as u32);
            if cap > 1 { let p = crate::fibre::store_ptr(); wstr(unsafe { &mut *p }, dst as u32, &s, cap as usize) }
        })?;
    Ok(())
}
