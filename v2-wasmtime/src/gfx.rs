//! Real GLES3 on an X11 window through EGL.
//!
//! The engine's `gl*` imports go straight to GLES3; the guest pointers are the
//! guest's own buffers, which is exactly what GLES wants.  Mapped buffers are
//! the only translation: WebGL hands back a guest buffer, so a map mallocs one,
//! copies, and an unmap copies back.
use std::collections::HashMap;
use std::ffi::{c_void, CString};
use std::sync::Mutex;
use wasmtime::{AsContext, AsContextMut, StoreContextMut};

use khronos_egl as egl;
use x11_dl::xlib;

pub const W: i32 = 1280;
pub const H: i32 = 720;

type Egl = egl::DynamicInstance<egl::EGL1_5>;

struct Map {
    guest: u32,
    host: *mut c_void,
    len: usize,
}

struct Gfx {
    xlib: xlib::Xlib,
    dpy: *mut xlib::Display,
    win: xlib::Window,
    egl: Egl,
    edpy: egl::Display,
    surface: egl::Surface,
    ctx: egl::Context,
    cfg: egl::Config,
    procs: HashMap<String, usize>,
    maps: HashMap<u32, Map>,
    w: i32,
    h: i32,
    px: i32,
    py: i32,
    wm_delete: xlib::Atom,
}

unsafe impl Send for Gfx {}

static GFX: Mutex<Option<Gfx>> = Mutex::new(None);
static INIT: Mutex<()> = Mutex::new(());

thread_local! {
    static CURRENT: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

/// Open the window and make a GLES3 context.  Idempotent.
pub fn ensure() -> Result<(), String> {
    if GFX.lock().unwrap().is_some() {
        return Ok(());
    }
    let _g = INIT.lock().unwrap();
    if GFX.lock().unwrap().is_some() {
        return Ok(());
    }
    unsafe { open() }
}

pub fn handles() -> Option<(*mut c_void, u64)> {
    let g = GFX.lock().unwrap();
    g.as_ref().map(|g| (g.dpy as *mut c_void, g.win as u64))
}

pub fn block<F: std::future::Future>(f: F) -> F::Output { pollster_block(f) }

fn wgpu_probe(g: &Gfx, screen: i32) {
    use raw_window_handle::{RawDisplayHandle, RawWindowHandle, XlibDisplayHandle, XlibWindowHandle};
    let inst = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle());
    let mut wh = XlibWindowHandle::new(g.win);
    wh.visual_id = 0;
    let raw = RawWindowHandle::Xlib(wh);
    let disp = RawDisplayHandle::Xlib(XlibDisplayHandle::new(std::ptr::NonNull::new(g.dpy as *mut _), screen));
    let surf = unsafe {
        inst.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
            raw_window_handle: raw,
            raw_display_handle: Some(disp),
        })
    };
    let surf = match surf { Ok(s) => s, Err(e) => { eprintln!("[wgpu] surface: {e}"); return } };
    let ad = match pollster_block(inst.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::LowPower,
        compatible_surface: Some(&surf),
        force_fallback_adapter: false,
        ..Default::default()
    })) {
        Ok(a) => a,
        Err(e) => { eprintln!("[wgpu] adapter: {e}"); return }
    };
    let info = ad.get_info();
    eprintln!("[wgpu] adapter {} {:?} {:?}", info.name, info.backend, info.device_type);
    match pollster_block(ad.request_device(&wgpu::DeviceDescriptor::default())) {
        Ok((dev, _q)) => {
            let caps = surf.get_capabilities(&ad);
            eprintln!("[wgpu] device ok formats={:?} present={:?}", caps.formats, caps.present_modes);
            drop((dev, _q));
        }
        Err(e) => eprintln!("[wgpu] device: {e}"),
    }
}

/// Minimal executor for the one-shot wgpu setup futures.
fn pollster_block<F: std::future::Future>(mut f: F) -> F::Output {
    use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};
    static VT: RawWakerVTable = RawWakerVTable::new(|_| RawWaker::new(std::ptr::null(), &VT), |_| {}, |_| {}, |_| {});
    let w = unsafe { Waker::from_raw(RawWaker::new(std::ptr::null(), &VT)) };
    let mut cx = Context::from_waker(&w);
    let mut f = unsafe { std::pin::Pin::new_unchecked(&mut f) };
    loop {
        match f.as_mut().poll(&mut cx) {
            Poll::Ready(v) => return v,
            Poll::Pending => std::thread::yield_now(),
        }
    }
}

unsafe fn open() -> Result<(), String> {
    let xlib = xlib::Xlib::open().map_err(|e| format!("libX11: {e}"))?;
    let dpy = (xlib.XOpenDisplay)(std::ptr::null());
    if dpy.is_null() {
        return Err("XOpenDisplay failed".into());
    }
    let screen = (xlib.XDefaultScreen)(dpy);
    let root = (xlib.XRootWindow)(dpy, screen);
    let win = (xlib.XCreateSimpleWindow)(dpy, root, 0, 0, W as u32, H as u32, 0, 0, 0);
    let title = CString::new("alcohol").unwrap();
    (xlib.XStoreName)(dpy, win, title.as_ptr());
    (xlib.XSelectInput)(
        dpy,
        win,
        xlib::ExposureMask
            | xlib::KeyPressMask
            | xlib::KeyReleaseMask
            | xlib::ButtonPressMask
            | xlib::ButtonReleaseMask
            | xlib::PointerMotionMask
            | xlib::StructureNotifyMask
            | xlib::FocusChangeMask
            | xlib::EnterWindowMask
            | xlib::LeaveWindowMask,
    );
    let mut wm_delete = (xlib.XInternAtom)(dpy, c"WM_DELETE_WINDOW".as_ptr(), 0);
    (xlib.XSetWMProtocols)(dpy, win, &mut wm_delete as *mut u64, 1);
    (xlib.XMapWindow)(dpy, win);
    (xlib.XFlush)(dpy);

    let lib = libloading::Library::new("libEGL.so.1").map_err(|e| format!("libEGL: {e}"))?;
    let egl = egl::DynamicInstance::<egl::EGL1_5>::load_required_from(lib)
        .map_err(|e| format!("egl load: {e:?}"))?;
    let edpy = match unsafe { egl.get_platform_display(0x31D5, dpy as *mut c_void, &[]) } {
        Ok(d) => d,
        Err(e) => {
            eprintln!("[gfx ] platform display: {e:?}; falling back");
            egl.get_display(dpy as *mut c_void).ok_or("eglGetDisplay")?
        }
    };
    egl.initialize(edpy).map_err(|e| format!("eglInitialize: {e:?}"))?;
    egl.bind_api(egl::OPENGL_ES_API).map_err(|e| format!("eglBindAPI: {e:?}"))?;
    let ven = egl.query_string(Some(edpy), egl::VENDOR).map(|c| c.to_string_lossy().into_owned()).unwrap_or_default();
    let ver = egl.query_string(Some(edpy), egl::VERSION).map(|c| c.to_string_lossy().into_owned()).unwrap_or_default();
    eprintln!("[gfx ] egl {ven} / {ver}");
    let mut allc: Vec<egl::Config> = Vec::with_capacity(egl.get_config_count(edpy).unwrap_or(0));
    let _ = egl.get_configs(edpy, &mut allc);
    eprintln!("[gfx ] configs={} err={:?}", allc.len(), egl.get_error());
    let mut cfgs: Vec<egl::Config> = Vec::with_capacity(32);
    let full: &[egl::Int] = &[
        egl::SURFACE_TYPE, egl::WINDOW_BIT,
        egl::RENDERABLE_TYPE, egl::OPENGL_ES3_BIT,
        egl::RED_SIZE, 8, egl::GREEN_SIZE, 8, egl::BLUE_SIZE, 8, egl::ALPHA_SIZE, 8,
        egl::DEPTH_SIZE, 24, egl::STENCIL_SIZE, 8, egl::NONE,
    ];
    let mut err = egl.choose_config(edpy, full, &mut cfgs);
    if cfgs.is_empty() {
        eprintln!("[gfx ] full config: {err:?}");
        let min: &[egl::Int] = &[egl::SURFACE_TYPE, egl::WINDOW_BIT, egl::RENDERABLE_TYPE, egl::OPENGL_ES3_BIT, egl::NONE];
        cfgs = Vec::with_capacity(32);
        err = egl.choose_config(edpy, min, &mut cfgs);
        if cfgs.is_empty() {
            eprintln!("[gfx ] minimal config: {err:?}");
            let any: &[egl::Int] = &[egl::SURFACE_TYPE, egl::WINDOW_BIT, egl::NONE];
            cfgs = Vec::with_capacity(32);
            let _ = egl.choose_config(edpy, any, &mut cfgs);
            eprintln!("[gfx ] any configs={}", cfgs.len());
        }
    }
    let cfg = *cfgs.first().ok_or("no EGL config")?;
    let ctx = egl
        .create_context(
            edpy,
            cfg,
            None,
            &[egl::CONTEXT_MAJOR_VERSION, 3, egl::CONTEXT_MINOR_VERSION, 0, egl::NONE],
        )
        .map_err(|e| format!("eglCreateContext: {e:?}"))?;
    let surface = egl
        .create_window_surface(edpy, cfg, win as egl::NativeWindowType, None)
        .map_err(|e| format!("eglCreateWindowSurface: {e:?}"))?;
    egl.make_current(edpy, Some(surface), Some(surface), Some(ctx))
        .map_err(|e| format!("eglMakeCurrent: {e:?}"))?;
    CURRENT.with(|c| c.set(true));

    let mut gfx = Gfx {
        xlib, dpy, win, egl, edpy, surface, ctx, cfg,
        procs: HashMap::new(), maps: HashMap::new(), w: W, h: H, px: 0, py: 0, wm_delete,
    };
    if std::env::var_os("WC_GFXPROBE").is_some() {
        wgpu_probe(&gfx, screen);
    }
    let ver = gfx.gl_get(gl::VERSION);
    eprintln!("[gfx ] {} | {}", ver, gfx.gl_get(gl::RENDERER));
    *GFX.lock().unwrap() = Some(gfx);
    Ok(())
}

pub fn present() {
    let g = GFX.lock().unwrap();
    if let Some(g) = &*g {
        let _ = g.egl.swap_buffers(g.edpy, g.surface);
    }
}

pub fn make_current() {
    let g = GFX.lock().unwrap();
    if let Some(g) = &*g {
        if !CURRENT.with(|c| c.get()) {
            let _ = g.egl.make_current(g.edpy, Some(g.surface), Some(g.surface), Some(g.ctx));
            CURRENT.with(|c| c.set(true));
        }
    }
}

mod gl {
    pub const VENDOR: u32 = 0x1f00;
    pub const RENDERER: u32 = 0x1f01;
    pub const VERSION: u32 = 0x1f02;
}

impl Gfx {
    /// Resolve and cache one GL entry point.
    fn proc(&mut self, name: &str) -> Option<usize> {
        if let Some(p) = self.procs.get(name) {
            return Some(*p);
        }
        let f = self.egl.get_proc_address(name)?;
        let p = f as usize;
        self.procs.insert(name.to_string(), p);
        Some(p)
    }

    fn gl_get(&mut self, name: u32) -> String {
        let Some(p) = self.proc("glGetString") else { return String::new() };
        let f: extern "system" fn(u32) -> *const u8 = unsafe { std::mem::transmute(p) };
        let s = f(name);
        if s.is_null() {
            return String::new();
        }
        unsafe { std::ffi::CStr::from_ptr(s as *const _) }.to_string_lossy().into_owned()
    }

}

fn g_malloc(st: &mut StoreContextMut<'_, crate::state::St>, n: u32) -> u32 {
    if n == 0 {
        return 0;
    }
    let Some(inst) = st.data().inst else { return 0 };
    let Ok(f) = inst.get_typed_func::<(u32,), u32>(&mut *st, "malloc") else { return 0 };
    crate::fibre::run_sync(f.call_async(&mut *st, (n,))).unwrap_or(0)
}

fn g_free(st: &mut StoreContextMut<'_, crate::state::St>, p: u32) {
    if p == 0 {
        return;
    }
    let Some(inst) = st.data().inst else { return };
    if let Ok(f) = inst.get_typed_func::<(u32,), ()>(&mut *st, "free") {
        let _ = crate::fibre::run_sync(f.call_async(&mut *st, (p,)));
    }
}

/// Raw call: args are already marshalled as 64-bit words, one per wasm param.
pub fn call(name: &str, args: &[u64], st: &mut StoreContextMut<'_, crate::state::St>) -> u64 {
    make_current();
    let mut g = GFX.lock().unwrap();
    let Some(g) = &mut *g else { return 0 };
    let Some(p) = g.proc(name) else {
        eprintln!("[gfx ] missing {name}");
        return 0;
    };
    let f = unsafe { std::mem::transmute::<usize, extern "system" fn(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64) -> u64>(p) };

    // map/unmap need the guest buffer on both sides
    if name == "glMapBufferRange" {
        let (target, off, len, access) = (args[0] as u32, args[1] as u32 as i64, args[2] as u32 as i64, args[3] as u32);
        let gmap: extern "system" fn(u32, i64, i64, u32) -> *mut c_void = unsafe { std::mem::transmute(p) };
        let host = gmap(target, off, len, access);
        if host.is_null() {
            return 0;
        }
        let gp = g_malloc(st, len as u32);
        if gp == 0 {
            return 0;
        }
        let mem = st.data().mem.clone();
        let d = mem.data();
        unsafe { std::ptr::copy_nonoverlapping(host as *const u8, d[gp as usize].get(), len as usize) };
        g.maps.insert(target, Map { guest: gp, host, len: len as usize });
        return gp as u64;
    }
    if name == "glUnmapBuffer" {
        let target = args[0] as u32;
        let unmap: extern "system" fn(u32) -> u8 = unsafe { std::mem::transmute(p) };
        if let Some(m) = g.maps.remove(&target) {
            let mem = st.data().mem.clone();
            let d = mem.data();
            unsafe { std::ptr::copy_nonoverlapping(d[m.guest as usize].get(), m.host as *mut u8, m.len) };
            g_free(st, m.guest);
        }
        return unmap(target) as u64;
    }

    f(
        args.first().copied().unwrap_or(0),
        args.get(1).copied().unwrap_or(0),
        args.get(2).copied().unwrap_or(0),
        args.get(3).copied().unwrap_or(0),
        args.get(4).copied().unwrap_or(0),
        args.get(5).copied().unwrap_or(0),
        args.get(6).copied().unwrap_or(0),
        args.get(7).copied().unwrap_or(0),
        args.get(8).copied().unwrap_or(0),
        args.get(9).copied().unwrap_or(0),
        args.get(10).copied().unwrap_or(0),
        args.get(11).copied().unwrap_or(0),
    )
}

/// Guest pointer to a NUL-terminated copy of `s` (glGetString).
pub fn str_to_guest(st: &mut StoreContextMut<'_, crate::state::St>, s: &str) -> u32 {
    let p = g_malloc(st, s.len() as u32 + 1);
    if p != 0 {
        let mem = st.data().mem.clone();
        let d = mem.data();
        unsafe { std::ptr::copy_nonoverlapping(s.as_ptr(), d[p as usize].get(), s.len()) };
        unsafe { *d[p as usize + s.len()].get() = 0 };
    }
    p
}

/// Drain window events: resize the surface, close on WM_DELETE_WINDOW.
pub fn poll() {
    let mut g = GFX.lock().unwrap();
    let Some(g) = &mut *g else { return };
    let mut ev: xlib::XEvent = unsafe { std::mem::zeroed() };
    while unsafe { (g.xlib.XPending)(g.dpy) } > 0 {
        unsafe { (g.xlib.XNextEvent)(g.dpy, &mut ev) };
        match unsafe { ev.get_type() } {
            xlib::ConfigureNotify => {
                let c = unsafe { ev.configure };
                if c.width as i32 != g.w || c.height as i32 != g.h {
                    g.w = c.width as i32;
                    g.h = c.height as i32;
                    let _ = g.egl.make_current(g.edpy, None, None, None);
                    let _ = g.egl.destroy_surface(g.edpy, g.surface);
                    if let Ok(s) = unsafe { g.egl.create_window_surface(g.edpy, g.cfg, g.win as egl::NativeWindowType, None) } {
                        g.surface = s;
                    }
                    let _ = g.egl.make_current(g.edpy, Some(g.surface), Some(g.surface), Some(g.ctx));
                }
            }
            xlib::KeyPress | xlib::KeyRelease => {
                let mut k = unsafe { ev.key };
                let down = unsafe { ev.get_type() } == xlib::KeyPress;
                let ks = unsafe { (g.xlib.XLookupKeysym)(&mut k, 0) };
                let (key, code, kc, ch) = keysym_name(ks);
                crate::imports::input::push(crate::imports::input::Ev::Key {
                    down, key, code, keycode: kc,
                    ch: if (32..127).contains(&ch) { ch } else { 0 },
                    modifiers: k.state as u32, repeat: false,
                });
            }
            xlib::ButtonPress | xlib::ButtonRelease => {
                let b = unsafe { ev.button };
                let down = unsafe { ev.get_type() } == xlib::ButtonPress;
                if b.button == 4 || b.button == 5 {
                    crate::imports::input::push(crate::imports::input::Ev::Wheel {
                        x: b.x as f64, y: b.y as f64, dx: 0.0,
                        dy: if b.button == 4 { 1.0 } else { -1.0 },
                        mode: 0, modifiers: b.state as u32,
                    });
                } else {
                    crate::imports::input::push(crate::imports::input::Ev::Mouse {
                        down: if down { Some((b.button.saturating_sub(1)) as u16) } else { None },
                        x: b.x as f64, y: b.y as f64, mx: 0.0, my: 0.0,
                        button: (b.button.saturating_sub(1)) as u16, modifiers: b.state as u32,
                    });
                }
            }
            xlib::MotionNotify => {
                let m = unsafe { ev.motion };
                crate::imports::input::push(crate::imports::input::Ev::Mouse {
                    down: None, x: m.x as f64, y: m.y as f64,
                    mx: (m.x - g.px) as f64, my: (m.y - g.py) as f64,
                    button: 0, modifiers: m.state as u32,
                });
                g.px = m.x; g.py = m.y;
            }
            xlib::FocusIn => crate::imports::input::push(crate::imports::input::Ev::Focus { on: true }),
            xlib::FocusOut => crate::imports::input::push(crate::imports::input::Ev::Focus { on: false }),
            xlib::ClientMessage => {
                let c = unsafe { ev.client_message };
                if c.data.get_long(0) as xlib::Atom == g.wm_delete {
                    std::process::exit(0);
                }
            }
            _ => {}
        }
    }
}

pub fn shot(path: &str) {
    let g = GFX.lock().unwrap();
    let Some(g) = &*g else { return };
    let img = unsafe { (g.xlib.XGetImage)(g.dpy, g.win, 0, 0, g.w as u32, g.h as u32, !0, xlib::ZPixmap) };
    if img.is_null() { eprintln!("[shot] XGetImage failed"); return }
    let mut out = format!("P6\n{} {}\n255\n", g.w, g.h).into_bytes();
    for y in 0..g.h {
        for x in 0..g.w {
            let px: u64 = unsafe { (g.xlib.XGetPixel)(img, x, y) };
            // 32-bit BGRX
            out.push(((px >> 16) & 0xff) as u8);
            out.push(((px >> 8) & 0xff) as u8);
            out.push((px & 0xff) as u8);
        }
    }
    unsafe { (g.xlib.XDestroyImage)(img) };
    let _ = std::fs::write(path, out);
    eprintln!("[shot] wrote {path}");
}

pub fn pointer_lock(on: bool) {
    let g = GFX.lock().unwrap();
    let Some(g) = &*g else { return };
    if on {
        unsafe { (g.xlib.XGrabPointer)(g.dpy, g.win, 1, 0, xlib::GrabModeAsync, xlib::GrabModeAsync, g.win, 0, xlib::CurrentTime) };
    } else {
        unsafe { (g.xlib.XUngrabPointer)(g.dpy, xlib::CurrentTime) };
    }
    unsafe { (g.xlib.XFlush)(g.dpy) };
}

fn keysym_name(ks: xlib::KeySym) -> (String, String, u32, u32) {
    let v = ks as u64;
    let key = match ks {
        0xff0d => ("Enter".into(), "Enter".into(), 13, 13),
        0xff08 => ("Backspace".into(), "Backspace".into(), 8, 8),
        0xff09 => ("Tab".into(), "Tab".into(), 9, 9),
        0xff1b => ("Escape".into(), "Escape".into(), 27, 27),
        0x20 => (" ".into(), "Space".into(), 32, 32),
        0xffe1 | 0xffe2 => ("Shift".into(), "ShiftLeft".into(), 16, 0),
        0xffe3 | 0xffe4 => ("Control".into(), "ControlLeft".into(), 17, 0),
        0xffe9 | 0xffea => ("Alt".into(), "AltLeft".into(), 18, 0),
        0xffeb | 0xffec => ("Meta".into(), "MetaLeft".into(), 91, 0),
        0xff51 => ("ArrowLeft".into(), "ArrowLeft".into(), 37, 0),
        0xff53 => ("ArrowRight".into(), "ArrowRight".into(), 39, 0),
        0xff52 => ("ArrowUp".into(), "ArrowUp".into(), 38, 0),
        0xff54 => ("ArrowDown".into(), "ArrowDown".into(), 40, 0),
        0xffff => ("Delete".into(), "Delete".into(), 46, 0),
        0xff50 => ("Home".into(), "Home".into(), 36, 0),
        0xff57 => ("End".into(), "End".into(), 35, 0),
        0xff55 => ("PageUp".into(), "PageUp".into(), 33, 0),
        0xff56 => ("PageDown".into(), "PageDown".into(), 34, 0),
        0xffbe..=0xffc9 => {
            let n = (v - 0xffbe + 1) as u32;
            (format!("F{n}"), format!("F{n}"), 111 + n, 0)
        }
        _ => {
            if (0x20..0x7f).contains(&v) {
                let ch = char::from_u32(v as u32).unwrap_or('?');
                (ch.to_string(), format!("Key{}", ch.to_ascii_uppercase()), v as u32, v as u32)
            } else {
                (String::new(), String::new(), 0, 0)
            }
        }
    };
    key
}

pub fn size() -> (i32, i32) {
    let g = GFX.lock().unwrap();
    g.as_ref().map(|g| (g.w, g.h)).unwrap_or((W, H))
}
