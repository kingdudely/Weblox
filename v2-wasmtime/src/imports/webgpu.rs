//! WebGPU imports, for real: they drive wgpu on the same X11 window.
//!
//! The module talks the webgpu.h C ABI.  Descriptor layouts below follow the
//! glue's own readers (they are written out in the shipped WebClient.js), so
//! the two sides agree field for field.  Guest linear memory is host memory:
//! data pointers are passed straight through, and mapped ranges are copied into
//! guest buffers because WebGPU hands JS a new ArrayBuffer.
use std::collections::HashMap;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};

use wasmtime::{AsContextMut, Caller, FuncType, Linker, Val, ValType};

use crate::imports::wgt;
use crate::state::St;

const I: ValType = ValType::I32;
const L: ValType = ValType::I64;
const F: ValType = ValType::F32;

enum Obj {
    Device(wgpu::Device),
    Queue(wgpu::Queue),
    Buffer(wgpu::Buffer),
    Texture(wgpu::Texture),
    View(wgpu::TextureView),
    Sampler(wgpu::Sampler),
    Shader(wgpu::ShaderModule),
    Bgl(wgpu::BindGroupLayout),
    Bg(wgpu::BindGroup),
    Pl(wgpu::PipelineLayout),
    Rp(wgpu::RenderPipeline),
    Cp(wgpu::ComputePipeline),
    Enc(Option<wgpu::CommandEncoder>),
    Cmd(wgpu::CommandBuffer),
    RPass(Option<(wgpu::RenderPass<'static>, Vec<Box<wgpu::TextureView>>)>),
    CPass(Option<wgpu::ComputePass<'static>>),
    Surface(wgpu::Surface<'static>),
    Query(wgpu::QuerySet),
    Bundle(wgpu::RenderBundle),
    BundleEnc(wgpu::RenderBundleEncoder<'static>),
    Other,
}

#[derive(Default)]
struct Gpu {
    objs: HashMap<u32, Obj>,
    device: Option<wgpu::Device>,
    queue: Option<wgpu::Queue>,
    surface: Option<wgpu::Surface<'static>>,
    adapter: Option<wgpu::Adapter>,
    maps: HashMap<u32, (u32, usize, bool)>,
    size: (u32, u32),
}

static GPU: Mutex<Option<Gpu>> = Mutex::new(None);
unsafe impl Send for Obj {}
unsafe impl Sync for Obj {}
static NEXT: AtomicU32 = AtomicU32::new(0x1000);
static INIT: Mutex<()> = Mutex::new(());

fn with<R>(f: impl FnOnce(&mut Gpu) -> R) -> R {
    let mut g = GPU.lock().unwrap();
    f(g.get_or_insert_with(Default::default))
}

fn put(o: Obj) -> u32 {
    let id = NEXT.fetch_add(1, Ordering::Relaxed);
    with(|g| g.objs.insert(id, o));
    id
}

fn put_at(id: u32, o: Obj) {
    with(|g| g.objs.insert(id, o));
}

/// Allocate the C++ wrapper half of a handle by calling its export.
fn wrapped(st: &mut wasmtime::Store<St>, name: &str, o: Obj) -> i32 {
    let w = wrapper(st, name);
    if w != 0 { put_at(w, o) }
    w as i32
}

fn wrapper(st: &mut wasmtime::Store<St>, name: &str) -> u32 {
    let Some(inst) = st.data().inst else { return 0 };
    let Ok(f) = inst.get_typed_func::<(i32,), i32>(&mut *st, name) else { return 0 };
    crate::fibre::run_sync(f.call_async(&mut *st, (0,))).unwrap_or(0) as u32
}

fn take(id: u32) -> Option<Obj> {
    with(|g| g.objs.remove(&id))
}

fn takeh(id: i32) -> Option<Obj> {
    take(id as u32)
}

fn dev(g: &Gpu) -> Option<wgpu::Device> {
    g.device.clone()
}

fn poll_device() {
    with(|g| {
        if let Some(d) = &g.device {
            let _ = d.poll(wgpu::PollType::Wait { submission_index: None, timeout: None });
        }
    });
}

/// Read helpers over guest memory.
struct M<'a> {
    d: &'a [std::cell::UnsafeCell<u8>],
}

trait Off { fn o(self) -> u32; }
impl Off for i32 { fn o(self) -> u32 { self as u32 } }
impl Off for u32 { fn o(self) -> u32 { self } }

impl<'a> M<'a> {
    fn u32(&self, p: impl Off) -> u32 {
        let p = p.o();
        if p == 0 { return 0 }
        unsafe { *(self.d[p as usize].get() as *const u32) }
    }
    fn i32(&self, p: impl Off) -> i32 { self.u32(p) as i32 }
    fn u64(&self, p: impl Off) -> u64 {
        let p = p.o();
        (self.u32(p) as u64) | ((self.u32(p.wrapping_add(4)) as u64) << 32)
    }
    fn f32(&self, p: impl Off) -> f32 { f32::from_bits(self.u32(p)) }
    fn bytes(&self, p: impl Off, n: usize) -> &'a [u8] {
        let p = p.o();
        if p == 0 || n == 0 { return &[] }
        unsafe { std::slice::from_raw_parts(self.d[p as usize].get() as *const u8, n) }
    }
    fn buf(&self, p: impl Off, n: usize) -> Vec<u8> { self.bytes(p, n).to_vec() }
    fn str(&self, p: impl Off) -> String {
        let p = p.o();
        let sp = self.u32(p);
        let len = self.u32(p + 4);
        if sp == 0 { return String::new() }
        if len == u32::MAX {
            // -1 length: NUL-terminated, as the glue's T() scans
            let mut out = Vec::new();
            for i in sp as usize..self.d.len() {
                let b = unsafe { *self.d[i].get() };
                if b == 0 { break }
                out.push(b);
            }
            return String::from_utf8_lossy(&out).into_owned();
        }
        let n = (len as usize).min(4096);
        String::from_utf8_lossy(self.bytes(sp as i32, n)).into_owned()
    }
    fn opt_str(&self, p: impl Off) -> Option<String> {
        let s = self.str(p);
        if s.is_empty() { None } else { Some(s) }
    }
}

fn mem_of(st: &wasmtime::Store<St>) -> M<'static> {
    let d = st.data().mem.data();
    M { d: unsafe { std::mem::transmute::<&[std::cell::UnsafeCell<u8>], &'static [std::cell::UnsafeCell<u8>]>(d) } }
}

fn raw(st: &mut wasmtime::Store<St>, p: u32, n: usize) -> Vec<u8> {
    let m = st.data().mem.clone();
    let d = m.data();
    unsafe { std::slice::from_raw_parts(d[p as usize].get() as *const u8, n) }.to_vec()
}

fn g_malloc(st: &mut wasmtime::Store<St>, n: u32) -> u32 {
    let Some(inst) = st.data().inst else { return 0 };
    let Ok(f) = inst.get_typed_func::<(u32,), u32>(&mut *st, "malloc") else { return 0 };
    crate::fibre::run_sync(f.call_async(&mut *st, (n,))).unwrap_or(0)
}

fn g_free(st: &mut wasmtime::Store<St>, p: u32) {
    let Some(inst) = st.data().inst else { return };
    if let Ok(f) = inst.get_typed_func::<(u32,), ()>(&mut *st, "free") {
        let _ = crate::fibre::run_sync(f.call_async(&mut *st, (p,)));
    }
}

/// Create instance/device/surface bound to the window.
fn ensure() -> bool {
    if GPU.lock().unwrap().as_ref().map(|g| g.device.is_some()).unwrap_or(false) {
        return true;
    }
    let _i = INIT.lock().unwrap();
    if GPU.lock().unwrap().as_ref().map(|g| g.device.is_some()).unwrap_or(false) {
        return true;
    }
    if let Err(e) = crate::gfx::ensure() {
        eprintln!("[wgpu] window: {e}");
        return false;
    }
    let (dpy, win) = match crate::gfx::handles() {
        Some(v) => v,
        None => return false,
    };
    use raw_window_handle::{RawDisplayHandle, RawWindowHandle, XlibDisplayHandle, XlibWindowHandle};
    let inst = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle());
    let mut wh = XlibWindowHandle::new(win);
    wh.visual_id = 0;
    let dh = XlibDisplayHandle::new(std::ptr::NonNull::new(dpy as *mut _), 0);
    let surface = unsafe {
        inst.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
            raw_window_handle: RawWindowHandle::Xlib(wh),
            raw_display_handle: Some(RawDisplayHandle::Xlib(dh)),
        })
    };
    let surface = match surface {
        Ok(s) => s,
        Err(e) => { eprintln!("[wgpu] surface: {e}"); return false }
    };
    let adapter = match crate::gfx::block(inst.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        compatible_surface: Some(&surface),
        force_fallback_adapter: false,
        ..Default::default()
    })) {
        Ok(a) => a,
        Err(e) => { eprintln!("[wgpu] adapter: {e}"); return false }
    };
    let info = adapter.get_info();
    eprintln!("[wgpu] {} {:?}", info.name, info.backend);
    let (device, queue) = match crate::gfx::block(adapter.request_device(&wgpu::DeviceDescriptor {
        label: Some("alcohol"),
        required_features: adapter.features() & !wgpu::Features::EXPERIMENTAL_COOPERATIVE_MATRIX,
        required_limits: adapter.limits(),
        ..Default::default()
    })) {
        Ok(v) => v,
        Err(e) => { eprintln!("[wgpu] device: {e}"); return false }
    };
    device.on_uncaptured_error(std::sync::Arc::new(|e| eprintln!("[wgpu] error: {e}")));
    let dev = put(Obj::Device(device.clone()));
    let q = put(Obj::Queue(queue.clone()));
    with(|g| {
        g.device = Some(device);
        g.queue = Some(queue);
        g.adapter = Some(adapter);
        g.size = (crate::gfx::size().0 as u32, crate::gfx::size().1 as u32);
        g.objs.insert(0, Obj::Surface(surface));
        let _ = (dev, q);
    });
    true
}

pub fn register(l: &mut Linker<St>) -> wasmtime::Result<()> {
    let eng = l.engine().clone();
    l.func_wrap("env", "emscripten_webgpu_get_device", || -> i32 {
        crate::trace::hit("webgpu_device");
        if std::env::var_os("WC_GLES").is_some() { return 0 }
        if !ensure() { return 0 }
        let raw = with(|g| g.objs.iter().find(|(_, o)| matches!(o, Obj::Device(_))).map(|(k, _)| *k).unwrap_or(0));
        if raw == 0 { return 0 }
        let st = crate::fibre::store_ptr();
        let w = unsafe { wrapper(&mut *st, "emwgpuCreateDevice") };
        if w != 0 { if let Some(o) = take(raw) { put_at(w, o) } }
        w as i32
    })?;
    l.func_wrap("env", "emwgpuGetPreferredFormat", || -> i32 { 22 })?; // rgba8unorm

    macro_rules! f {
        ($name:literal, ($($p:ident : $t:ty),*), $r:ty, $body:expr) => {
            l.func_wrap("env", $name, |mut c: Caller<'_, St>, $($p: $t),*| -> $r {
                crate::trace::hit($name);
                let p = crate::fibre::store_ptr();
                let st = unsafe { &mut *p };
                st.data_mut().imp_last = $name;
                let m = mem_of(&*st);
                ($body)(st, m)
            })?;
        };
    }

    f!("wgpuInstanceCreateSurface", (_i: i32, _d: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if !ensure() { 0 } else { with(|g| g.objs.iter().find(|(_, o)| matches!(o, Obj::Surface(_))).map(|(k, _)| *k as i32).unwrap_or(0)) }
    });

    f!("wgpuSurfaceConfigure", (s: i32, d: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let (dev, fmt, w, h) = (m.u32(d + 4), m.u32(d + 8), m.u32(d + 24), m.u32(d + 28));
        let vf: Vec<wgpu::TextureFormat> = (0..m.u32(d + 32))
            .filter_map(|k| wgt::tex_format(m.u32((m.u32(d + 36) + k * 4) as i32)))
            .collect();
        with(|g| {
            let _ = dev;
            let Some(device) = g.device.clone() else { return };
            let Some(Obj::Surface(s)) = g.objs.get(&(s as u32)) else { return };
            let caps = s.get_capabilities(&g.adapter.as_ref().unwrap());
            let format = wgt::tex_format(fmt).filter(|f| caps.formats.contains(f))
                .or_else(|| caps.formats.first().copied()).unwrap_or(wgpu::TextureFormat::Bgra8Unorm);
            if w != 0 { g.size.0 = w }
            if h != 0 { g.size.1 = h }
            let conf = wgpu::SurfaceConfiguration {
                color_space: wgpu::SurfaceColorSpace::Srgb,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                format,
                width: g.size.0.max(1),
                height: g.size.1.max(1),
                present_mode: caps.present_modes.first().copied().unwrap_or(wgpu::PresentMode::Fifo),
                alpha_mode: caps.alpha_modes.first().copied().unwrap_or(wgpu::CompositeAlphaMode::Auto),
                view_formats: vec![],
                desired_maximum_frame_latency: 2,
            };
            let _ = device;
            s.configure(&device, &conf);
        });
    });

    f!("wgpuSurfaceGetCurrentTexture", (_s: i32, _t: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {});

    macro_rules! create {
        ($name:literal, $desc:ident, $body:expr) => {
            f!($name, ($d: i32, $desc: i32), i32, {
                let dev = match g_device() { Some(d) => d, None => return 0 };
                let r: Option<Obj> = $body;
                match r { Some(o) => put(o), None => 0 }
            });
        };
    }

    f!("emwgpuDeviceCreateBuffer", (d: i32, desc: i32, out: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let size = m.u64(desc + 24);
        let mapped = m.u32(desc + 32) != 0;
        let usage = wgpu::BufferUsages::from_bits_truncate(m.u32(desc + 16));
        let label = m.opt_str(desc + 4);
        let Some(dev) = g_device() else { return 0 };
        let b = dev.create_buffer(&wgpu::BufferDescriptor {
            label: label.as_deref(), size, usage,
            mapped_at_creation: mapped,
        });
        put_at(out as u32, Obj::Buffer(b));
        unsafe { *(st.data().mem.data()[out as usize].get() as *mut u32) = out as u32 };
        1
    });

    f!("emwgpuDeviceCreateShaderModule", (d: i32, desc: i32, out: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let chain = m.u32(desc) as i32;
        let code = if chain != 0 && m.u32((chain + 4) as i32) == 2 {
            let p = m.u32(chain + 8);
            let n = m.u32(chain + 12) as usize;
            String::from_utf8_lossy(m.bytes(p as i32, n)).into_owned()
        } else { String::new() };
        let label = m.opt_str(desc + 4);
        let Some(dev) = g_device() else { return };
        let sh = dev.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: label.as_deref(),
            source: wgpu::ShaderSource::Wgsl(code.into()),
        });
        put_at(out as u32, Obj::Shader(sh));
        unsafe { *(st.data().mem.data()[out as usize].get() as *mut u32) = out as u32 };
    });

    f!("emwgpuShaderModuleGetCompilationInfo", (sh: i32, fut: i64, out: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        // the engine only reads count + first message; report zero messages
        let _ = (sh, fut);
        unsafe {
            let d = st.data().mem.data();
            *(d[out as usize + 4].get() as *mut u32) = 0;
        }
    });

    f!("wgpuDeviceCreateTexture", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
                let Some(dev) = g_device() else { return 0 };
                        let label = m.opt_str(desc + 4);
                let usage = wgpu::TextureUsages::from_bits_truncate(m.u32(desc + 16));
        let dim = wgt::texture_dimension(m.u32(desc + 24)).unwrap_or(wgpu::TextureDimension::D2);
        let (w, h, dep) = (m.u32(desc + 28), m.u32(desc + 32), m.u32(desc + 36));
        let format = wgt::tex_format(m.u32(desc + 40)).unwrap_or(wgpu::TextureFormat::Rgba8Unorm);
        let mips = m.u32(desc + 44).max(1);
        let samples = m.u32(desc + 48).max(1);
        let t = dev.create_texture(&wgpu::TextureDescriptor {
            label: label.as_deref(), size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: dep },
            mip_level_count: mips, sample_count: samples, dimension: dim, format, usage,
            view_formats: &[],
        });
                wrapped(st, "emwgpuCreateTexture", Obj::Texture(t))
    });

    f!("wgpuTextureCreateView", (t: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let Some(Obj::Texture(t)) = take(t as u32) else { return 0 };
        let r = if desc == 0 {
            t.create_view(&wgpu::TextureViewDescriptor::default())
        } else {
            let label = m.opt_str(desc + 4);
            let chain = m.u32(desc) as i32;
            let dim = if chain != 0 { wgt::view_dimension(m.u32(chain + 8)) } else { None };
            t.create_view(&wgpu::TextureViewDescriptor {
                label: label.as_deref(),
                dimension: dim,
                ..Default::default()
            })
        };
        put(Obj::Texture(t));
        put(Obj::View(r)) as i32
    });

    f!("wgpuDeviceCreateSampler", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let Some(dev) = g_device() else { return 0 };
        let s = dev.create_sampler(&wgpu::SamplerDescriptor {
            label: if desc != 0 { m.opt_str(desc + 4).as_deref().map(|s| s.to_string()) } else { None }.as_deref(),
            address_mode_u: wgt::address_mode(m.u32(desc + 12)).unwrap_or(wgpu::AddressMode::ClampToEdge),
            address_mode_v: wgt::address_mode(m.u32(desc + 16)).unwrap_or(wgpu::AddressMode::ClampToEdge),
            address_mode_w: wgt::address_mode(m.u32(desc + 20)).unwrap_or(wgpu::AddressMode::ClampToEdge),
            mag_filter: wgt::filter_mode(m.u32(desc + 24)).unwrap_or(wgpu::FilterMode::Nearest),
            min_filter: wgt::filter_mode(m.u32(desc + 28)).unwrap_or(wgpu::FilterMode::Nearest),
            mipmap_filter: wgt::mipmap_filter(m.u32(desc + 32)).unwrap_or(wgpu::MipmapFilterMode::Nearest),
            lod_min_clamp: m.f32(desc + 36),
            lod_max_clamp: m.f32(desc + 40),
            compare: wgt::compare(m.u32(desc + 44)),
            ..Default::default()
        });
        wrapped(st, "emwgpuCreateSampler", Obj::Sampler(s))
    });

    f!("wgpuDeviceCreateCommandEncoder", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = (d, desc);
        let Some(dev) = g_device() else { return 0 };
        put(Obj::Enc(Some(dev.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None })))) as i32
    });

    f!("wgpuCommandEncoderFinish", (e: i32, _d: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let Some(Obj::Enc(Some(enc))) = takeh(e) else { return 0 };
        put(Obj::Cmd(enc.finish())) as i32
    });

    f!("wgpuQueueWriteBuffer", (q: i32, b: i32, off: i64, data: i32, len: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = q;
        let bytes = raw(st, data as u32, len.max(0) as usize);
        with(|g| {
            let (Some(q), Some(Obj::Buffer(b))) = (&g.queue, g.objs.get(&(b as u32))) else { return };
            q.write_buffer(b, off as u64, &bytes);
        });
    });

    f!("wgpuQueueWriteTexture", (q: i32, dst: i32, data: i32, len: i32, layout: i32, extent: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = q;
        let bytes = raw(st, data as u32, len.max(0) as usize);
        if std::env::var_os("WC_WGPU").is_some() { eprintln!("[wgt ] len={len} tex={} mip={} ext={}x{}x{} bpr={} rows={}", m.u32(dst), m.u32(dst + 4), m.u32(extent), m.u32(extent + 4), m.u32(extent + 8), m.u32(layout + 8), m.u32(layout + 12)); }
        let tex = m.u32(dst);
        let mip = m.u32(dst + 4);
        let origin = wgpu::Origin3d { x: m.u32(dst + 8), y: m.u32(dst + 12), z: m.u32(dst + 16) };
        let rows = m.u32(layout + 12);
        let bpr = m.u32(layout + 8);
        let ext = wgpu::Extent3d { width: m.u32(extent), height: m.u32(extent + 4), depth_or_array_layers: m.u32(extent + 8) };
        with(|g| {
            let (Some(q), Some(Obj::Texture(t))) = (&g.queue, g.objs.get(&tex)) else { return };
            q.write_texture(
                wgpu::TexelCopyTextureInfo { texture: t, mip_level: mip, origin, aspect: wgpu::TextureAspect::All },
                &bytes,
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: if bpr == u32::MAX { None } else { Some(bpr) },
                    rows_per_image: if rows == u32::MAX { None } else { Some(rows) },
                },
                ext,
            );
        });
    });

    f!("wgpuCommandEncoderBeginRenderPass", (e: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let atts_n = m.u32(desc + 12);
        let atts_p = m.u32(desc + 16);
        let mut keep: Vec<Box<wgpu::TextureView>> = Vec::new();
        let mut ids: Vec<u32> = Vec::new();
        for k in 0..atts_n {
            let a = (atts_p + k * 56) as i32;
            let id = m.u32(a + 4);
            let v = with(|g| g.objs.get(&id).map(|o| match o { Obj::View(v) => v.clone(), _ => unreachable!() }));
            match v { Some(v) => { keep.push(Box::new(v)); ids.push(1) }, None => ids.push(0) }
        }
        let mut atts: Vec<Option<wgpu::RenderPassColorAttachment>> = Vec::new();
        for (k, ok) in ids.iter().enumerate() {
            let a = (atts_p + k as u32 * 56) as i32;
            if *ok == 0 { atts.push(None); continue }
            let clear = m.u32(a + 16);
            let color = wgpu::Color {
                r: m.f32(a + 24) as f64, g: m.f32(a + 28) as f64,
                b: m.f32(a + 32) as f64, a: m.f32(a + 36) as f64,
            };
            let load = if clear != 0 { wgpu::LoadOp::Clear(color) } else { wgpu::LoadOp::Load };
            atts.push(Some(wgpu::RenderPassColorAttachment {
                view: &keep[k],
                resolve_target: None,
                depth_slice: None,
                ops: wgpu::Operations { load, store: wgpu::StoreOp::Store },
            }));
        }
        let ds = m.u32(desc + 20) as i32;
        let depth = if ds != 0 {
            let v = m.u32(ds + 4);
            with(|g| g.objs.get(&v).map(|o| match o { Obj::View(v) => v.clone(), _ => unreachable!() }))
                .map(|v| (Box::new(v), m.f32(ds + 16)))
        } else { None };
        let Some(Obj::Enc(Some(mut enc))) = takeh(e) else { return 0 };
        let rp;
        {
            let mut rpd = wgpu::RenderPassDescriptor {
                label: None,
                color_attachments: &atts,
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            };
            let ds_att = depth.as_ref().map(|(v, clear)| wgpu::RenderPassDepthStencilAttachment {
                view: v,
                depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(*clear), store: wgpu::StoreOp::Store }),
                stencil_ops: None,
            });
            rpd.depth_stencil_attachment = ds_att;
            let p = enc.begin_render_pass(&rpd);
            rp = unsafe { std::mem::transmute::<wgpu::RenderPass<'_>, wgpu::RenderPass<'static>>(p) };
        }
        with(|g| { g.objs.insert(e as u32, Obj::Enc(Some(enc))); });
        let _ = &keep;
        wrapped(st, "emwgpuCreateRenderPassEncoder", Obj::RPass(Some((rp, keep))))
    });

    macro_rules! rpass {
        ($name:literal, ($($n:ident : $t:ty),*), |$pass:ident, $m:ident $(, $a:ident)*| $body:expr) => {
            f!($name, (p: i32 $(, $n: $t)*), (), |_st: &mut wasmtime::Store<St>, _m: M<'_>| {
                let Some(Obj::RPass(Some((mut $pass, keep)))) = with(|g| g.objs.remove(&(p as u32))) else { return };
                $body;
                with(|g| { g.objs.insert(p as u32, Obj::RPass(Some(($pass, keep)))); });
            });
        };
    }

    rpass!("wgpuRenderPassEncoderSetPipeline", (a: i32), |pass, m, a| {
        if let Some(rp) = with(|g| g.objs.get(&(a as u32)).cloned_rp()) { pass.set_pipeline(&rp) }
    });

    f!("wgpuRenderPassEncoderSetBindGroup", (p: i32, i: i32, b: i32, _n: i32, _o: i32), (), |_st: &mut wasmtime::Store<St>, _m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            if let Some(bg) = with(|g| g.objs.get(&(b as u32)).cloned_bg()) { pass.set_bind_group(i as u32, Some(&bg), &[]) }
            put(Obj::RPass(Some((pass, keep))));
        }
    });
    f!("wgpuRenderPassEncoderSetVertexBuffer", (p: i32, slot: i32, b: i32, off: i64, size: i64), (), |_st: &mut wasmtime::Store<St>, _m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            if let Some(buf) = with(|g| g.objs.get(&(b as u32)).cloned_buf()) {
                let end = if size as u64 == u64::MAX { buf.size() } else { off as u64 + size as u64 };
                pass.set_vertex_buffer(slot as u32, buf.slice(off as u64..end));
            }
            put(Obj::RPass(Some((pass, keep))));
        }
    });
    f!("wgpuRenderPassEncoderSetIndexBuffer", (p: i32, b: i32, fmt: i32, off: i64, size: i64), (), |_st: &mut wasmtime::Store<St>, _m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            if let Some(buf) = with(|g| g.objs.get(&(b as u32)).cloned_buf()) {
                let end = if size as u64 == u64::MAX { buf.size() } else { off as u64 + size as u64 };
                pass.set_index_buffer(buf.slice(off as u64..end), wgt::index_format(fmt as u32).unwrap_or(wgpu::IndexFormat::Uint32));
            }
            put(Obj::RPass(Some((pass, keep))));
        }
    });

    f!("wgpuRenderPassEncoderEnd", (p: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::RPass(Some((pass, _)))) = takeh(p) { drop(pass) }
    });
    f!("wgpuRenderPassEncoderDraw", (p: i32, v: i32, inst: i32, base: i32, n: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            pass.draw(v as u32..(v + n) as u32, base as u32..(base + inst) as u32);
            put(Obj::RPass(Some((pass, keep))));
        }
    });
    f!("wgpuRenderPassEncoderDrawIndexed", (p: i32, idx: i32, inst: i32, base: i32, vbase: i32, n: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            pass.draw_indexed(idx as u32..(idx + n) as u32, vbase, base as u32..(base + inst) as u32);
            put(Obj::RPass(Some((pass, keep))));
        }
    });
    f!("wgpuRenderPassEncoderSetViewport", (p: i32, x: f32, y: f32, w: f32, h: f32, n: f32, d: f32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            pass.set_viewport(x, y, w, h, n, d);
            put(Obj::RPass(Some((pass, keep))));
        }
    });
    f!("wgpuRenderPassEncoderSetScissorRect", (p: i32, x: i32, y: i32, w: i32, h: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            pass.set_scissor_rect(x as u32, y as u32, w as u32, h as u32);
            put(Obj::RPass(Some((pass, keep))));
        }
    });
    f!("wgpuRenderPassEncoderSetStencilReference", (p: i32, r: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            pass.set_stencil_reference(r as u32);
            put(Obj::RPass(Some((pass, keep))));
        }
    });
    f!("wgpuRenderPassEncoderPushDebugGroup", (p: i32, g: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = g;
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            pass.push_debug_group("");
            put(Obj::RPass(Some((pass, keep))));
        }
    });
    f!("wgpuRenderPassEncoderPopDebugGroup", (p: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::RPass(Some((mut pass, keep)))) = takeh(p) {
            pass.pop_debug_group();
            put(Obj::RPass(Some((pass, keep))));
        }
    });


    f!("wgpuDeviceCreateBindGroupLayout", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let Some(dev) = g_device() else { return 0 };
        let n = m.u32(desc + 12);
        let base = m.u32(desc + 16);
        let mut entries = Vec::new();
        for k in 0..n {
            let e = (base + k * 88) as i32;
            let binding = m.u32(e + 4);
            let vis = wgpu::ShaderStages::from_bits_truncate(m.u32(e + 8));
            let ty = match m.u32(e + 28) {
                1 | 2 | 3 => wgpu::BindingType::Buffer {
                    ty: match m.u32(e + 28) { 1 => wgpu::BufferBindingType::Uniform, 2 => wgpu::BufferBindingType::Storage { read_only: false }, _ => wgpu::BufferBindingType::Storage { read_only: true } },
                    has_dynamic_offset: m.u32(e + 32) != 0,
                    min_binding_size: wgpu::BufferSize::new(m.u64(e + 40)),
                },
                _ => match m.u32(e + 52) {
                    1 => wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    2 => wgpu::BindingType::Sampler(wgpu::SamplerBindingType::NonFiltering),
                    3 => wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),
                    _ => {
                        if m.u32(e + 60) != 0 {
                            let st = match m.u32(e + 60) {
                                1 => wgpu::TextureSampleType::Float { filterable: true },
                                2 => wgpu::TextureSampleType::Float { filterable: false },
                                3 => wgpu::TextureSampleType::Depth,
                                4 => wgpu::TextureSampleType::Sint,
                                _ => wgpu::TextureSampleType::Uint,
                            };
                            wgpu::BindingType::Texture {
                                sample_type: st,
                                view_dimension: wgt::view_dimension(m.u32(e + 64)).unwrap_or(wgpu::TextureViewDimension::D2),
                                multisampled: m.u32(e + 68) != 0,
                            }
                        } else {
                            let access = match m.u32(e + 76) { 1 => wgpu::StorageTextureAccess::WriteOnly, _ => wgpu::StorageTextureAccess::ReadOnly };
                            wgpu::BindingType::StorageTexture {
                                access,
                                format: wgt::tex_format(m.u32(e + 80)).unwrap_or(wgpu::TextureFormat::Rgba8Unorm),
                                view_dimension: wgt::view_dimension(m.u32(e + 84)).unwrap_or(wgpu::TextureViewDimension::D2),
                            }
                        }
                    }
                },
            };
            eprintln!("[wg] entry b={binding} vis={vis:?} ty={ty:?}");
            entries.push(wgpu::BindGroupLayoutEntry { binding, visibility: vis, ty, count: None });
        }
        let label = m.opt_str(desc + 4);
        eprintln!("[wg] bgl create n={n}");
        let o = dev.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: label.as_deref(), entries: &entries });
        eprintln!("[wg] bgl created, wrap");
        let r = wrapped(st, "emwgpuCreateBindGroupLayout", Obj::Bgl(o));
        eprintln!("[wg] bgl wrapped");
        r
    });

    f!("wgpuDeviceCreatePipelineLayout", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let Some(dev) = g_device() else { return 0 };
        let n = m.u32(desc + 12);
        let base = m.u32(desc + 16);
        let mut bgls = Vec::new();
        for k in 0..n {
            if let Some(b) = with(|g| g.objs.get(&(m.u32((base + k * 4) as i32) as u32)).cloned_bgl()) { bgls.push(b) }
        }
        let label = m.opt_str(desc + 4);
        let p = dev.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: label.as_deref(),
            bind_group_layouts: &bgls.iter().map(Some).collect::<Vec<_>>(),
            immediate_size: 0,
        });
        wrapped(st, "emwgpuCreatePipelineLayout", Obj::Pl(p))
    });

    f!("wgpuDeviceCreateBindGroup", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let Some(dev) = g_device() else { return 0 };
        let layout = m.u32(desc + 12);
        let Some(lay) = with(|g| g.objs.get(&layout).cloned_bgl()) else { return 0 };
        let n = m.u32(desc + 16) as usize;
        let base = m.u32(desc + 20);
        enum Res { Buf(wgpu::Buffer, u64, Option<wgpu::BufferSize>), Samp(wgpu::Sampler), View(wgpu::TextureView), None }
        let mut res: Vec<(u32, Res)> = Vec::with_capacity(n);
        for k in 0..n {
            let e = (base + k as u32 * 40) as i32;
            let binding = m.u32(e + 4);
            let r = if m.u32(e + 8) != 0 {
                let size = m.u64(e + 24);
                match with(|g| g.objs.get(&(m.u32(e + 8) as u32)).cloned_buf()) {
                    Some(b) => Res::Buf(b, m.u64(e + 16), if size == u64::MAX { None } else { wgpu::BufferSize::new(size) }),
                    None => Res::None,
                }
            } else if m.u32(e + 32) != 0 {
                match with(|g| g.objs.get(&(m.u32(e + 32) as u32)).cloned_sampler()) { Some(x) => Res::Samp(x), None => Res::None }
            } else {
                match with(|g| g.objs.get(&(m.u32(e + 36) as u32)).cloned_view()) { Some(x) => Res::View(x), None => Res::None }
            };
            res.push((binding, r));
        }
        let mut entries: Vec<wgpu::BindGroupEntry> = Vec::with_capacity(n);
        for (binding, r) in &res {
            match r {
                Res::Buf(b, off, size) => entries.push(wgpu::BindGroupEntry { binding: *binding, resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding { buffer: b, offset: *off, size: *size }) }),
                Res::Samp(s) => entries.push(wgpu::BindGroupEntry { binding: *binding, resource: wgpu::BindingResource::Sampler(s) }),
                Res::View(v) => entries.push(wgpu::BindGroupEntry { binding: *binding, resource: wgpu::BindingResource::TextureView(v) }),
                Res::None => {}
            }
        }
        let label = m.opt_str(desc + 4);
        put(Obj::Bg(dev.create_bind_group(&wgpu::BindGroupDescriptor { label: label.as_deref(), layout: &lay, entries: &entries }))) as i32
    });

    f!("wgpuDeviceCreateRenderPipeline", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let Some(dev) = g_device() else { return 0 };
        let label = m.opt_str(desc + 4);
        let layout = m.u32(desc + 12);
        let lay = with(|g| g.objs.get(&layout).cloned_pl());
        let consts = |p0: u32| -> Vec<(String, f64)> {
            let p = p0 as i32;
            let mut h = Vec::new();
            for k in 0..m.u32(p) {
                let e = (m.u32(p + 4) + k * 24) as i32;
                let s = m.str(e + 4);
                h.push((s, f64::from_bits(m.u64(e + 16))));
            }
            h
        };
        let vp = m.u32(desc + 16);
        let vmod = m.u32(vp + 4);
        let Some(vmod) = with(|g| g.objs.get(&vmod).cloned_shader()) else { return 0 };
        let ventry = m.str(vp + 8);
        let vconsts = consts(vp + 16);
        let vbufn = m.u32(vp + 24);
        let vbufp = m.u32(vp + 28);
        let mut attrsvec: Vec<Vec<wgpu::VertexAttribute>> = Vec::new();
        let mut layouts: Vec<wgpu::VertexBufferLayout> = Vec::new();
        for k in 0..vbufn {
            let b = vbufp + k as u32 * 24;
            let an = m.u32(b + 16);
            let ap = m.u32(b + 20);
            let mut at = Vec::new();
            for j in 0..an {
                let a = ap + j as u32 * 24;
                at.push(wgpu::VertexAttribute {
                    format: wgt::vertex_format(m.u32(a + 4)).unwrap_or(wgpu::VertexFormat::Float32x4),
                    offset: m.u64(a + 8), shader_location: m.u32(a + 16),
                });
            }
            attrsvec.push(at);
        }
        for (k, at) in attrsvec.iter().enumerate() {
            let b = vbufp + k as u32 * 24;
            layouts.push(wgpu::VertexBufferLayout {
                array_stride: m.u64(b + 8),
                step_mode: wgt::step_mode(m.u32(b + 4)).unwrap_or(wgpu::VertexStepMode::Vertex),
                attributes: at,
            });
        }
        let prim = (m.u32(desc + 48)) as i32;
        let primitive = wgpu::PrimitiveState {
            topology: wgt::topology(m.u32(prim + 4)).unwrap_or(wgpu::PrimitiveTopology::TriangleList),
            strip_index_format: wgt::index_format(m.u32(prim + 8)),
            front_face: wgt::front_face(m.u32(prim + 12)).unwrap_or(wgpu::FrontFace::Ccw),
            cull_mode: wgt::cull_mode(m.u32(prim + 16)),
            unclipped_depth: m.u32(prim + 20) != 0,
            ..Default::default()
        };
        let ms = (desc + 76) as u32;
        let multisample = wgpu::MultisampleState {
            count: m.u32(ms + 4).max(1), mask: m.u32(ms + 8) as u64,
            alpha_to_coverage_enabled: m.u32(ms + 12) != 0,
        };
        let dsp = m.u32(desc + 72);
        let depth_stencil = if dsp != 0 {
            let st = |p: u32| wgpu::StencilFaceState {
                compare: wgt::compare(m.u32(p)).unwrap_or(wgpu::CompareFunction::Always),
                fail_op: wgt::stencil_op(m.u32(p + 4)).unwrap_or(wgpu::StencilOperation::Keep),
                depth_fail_op: wgt::stencil_op(m.u32(p + 8)).unwrap_or(wgpu::StencilOperation::Keep),
                pass_op: wgt::stencil_op(m.u32(p + 12)).unwrap_or(wgpu::StencilOperation::Keep),
            };
            Some(wgpu::DepthStencilState {
                format: wgt::tex_format(m.u32(dsp + 4)).unwrap_or(wgpu::TextureFormat::Depth24Plus),
                depth_write_enabled: Some(m.u32(dsp + 8) != 0),
                depth_compare: Some(wgt::compare(m.u32(dsp + 12)).unwrap_or(wgpu::CompareFunction::Always)),
                stencil: wgpu::StencilState { front: st(dsp + 16), back: st(dsp + 32), read_mask: m.u32(dsp + 48) as u32, write_mask: m.u32(dsp + 52) as u32 },
                bias: wgpu::DepthBiasState {
                    constant: m.u32(dsp + 56) as i32,
                    slope_scale: m.f32(dsp + 60), clamp: m.f32(dsp + 64),
                },
            })
        } else { None };
        let fpp = m.u32(desc + 92);
        let mut fmod_o: Option<wgpu::ShaderModule> = None;
        let mut fentry = String::new();
        let mut fconsts: Vec<(String, f64)> = Vec::new();
        let mut targets: Vec<Option<wgpu::ColorTargetState>> = Vec::new();
        if fpp != 0 {
            let fmod = m.u32(fpp + 4);
            fmod_o = with(|g| g.objs.get(&fmod).cloned_shader());
            if fmod_o.is_none() { return 0 }
            fentry = m.str(fpp + 8);
            fconsts = consts(fpp + 16);
            let tn = m.u32(fpp + 24);
            let tp = m.u32(fpp + 28);
            for k in 0..tn {
                let t = tp + k as u32 * 24;
                let blendp = m.u32(t + 8);
                let bl = |p: u32| wgpu::BlendComponent {
                    src_factor: wgt::blend_factor(m.u32(p)).unwrap_or(wgpu::BlendFactor::One),
                    dst_factor: wgt::blend_factor(m.u32(p + 4)).unwrap_or(wgpu::BlendFactor::Zero),
                    operation: wgt::blend_op(m.u32(p + 8)).unwrap_or(wgpu::BlendOperation::Add),
                };
                let blend = if blendp != 0 { Some(wgpu::BlendState { color: bl(blendp), alpha: bl(blendp + 12) }) } else { None };
                targets.push(Some(wgpu::ColorTargetState {
                    format: wgt::tex_format(m.u32(t + 4)).unwrap_or(wgpu::TextureFormat::Rgba8Unorm),
                    blend, write_mask: wgpu::ColorWrites::from_bits_truncate(m.u32(t + 16)),
                }));
            }
        }
        let fconsts_s: Vec<(&str, f64)> = fconsts.iter().map(|(k, v)| (k.as_str(), *v)).collect();
        let vconsts_s: Vec<(&str, f64)> = vconsts.iter().map(|(k, v)| (k.as_str(), *v)).collect();
        let frag = fmod_o.as_ref().map(|fmod| wgpu::FragmentState {
            module: fmod,
            entry_point: Some(&fentry),
            compilation_options: wgpu::PipelineCompilationOptions { constants: &fconsts_s, ..Default::default() },
            targets: &targets,
        });
        let lbufs: Vec<Option<wgpu::VertexBufferLayout>> = layouts.into_iter().map(Some).collect();
        let rp = dev.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: label.as_deref(),
            layout: lay.as_ref(),
            vertex: wgpu::VertexState {
                module: &vmod, entry_point: Some(&ventry),
                compilation_options: wgpu::PipelineCompilationOptions { constants: &vconsts_s, ..Default::default() },
                buffers: &lbufs,
            },
            primitive,
            depth_stencil,
            multisample,
            fragment: frag,
            multiview_mask: None,
            cache: None,
        });
        wrapped(st, "emwgpuCreateRenderPipeline", Obj::Rp(rp))
    });

    f!("wgpuDeviceCreateComputePipeline", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let Some(dev) = g_device() else { return 0 };
        let layout = m.u32(desc + 12);
        let lay = with(|g| g.objs.get(&layout).cloned_pl());
        let c = m.u32(desc + 16);
        let Some(modu) = with(|g| g.objs.get(&m.u32(c + 4)).cloned_shader()) else { return 0 };
        let entry = m.str(c + 8);
        let p = dev.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: None, layout: lay.as_ref(),
            module: &modu, entry_point: Some(&entry),
            compilation_options: Default::default(), cache: None,
        });
        wrapped(st, "emwgpuCreateComputePipeline", Obj::Cp(p))
    });

    f!("wgpuCommandEncoderBeginComputePass", (e: i32, _d: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let Some(Obj::Enc(Some(mut enc))) = takeh(e) else { return 0 };
        let p = enc.begin_compute_pass(&wgpu::ComputePassDescriptor::default());
        let p = unsafe { std::mem::transmute::<wgpu::ComputePass<'_>, wgpu::ComputePass<'static>>(p) };
        with(|g| { g.objs.insert(e as u32, Obj::Enc(Some(enc))); });
        put(Obj::CPass(Some(p))) as i32
    });
    f!("wgpuComputePassEncoderSetPipeline", (p: i32, a: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::CPass(Some(mut pass))) = takeh(p) {
            if let Some(cp) = with(|g| g.objs.get(&(a as u32)).map(|o| match o { Obj::Cp(x) => x.clone(), _ => unreachable!() })) { pass.set_pipeline(&cp) }
            put(Obj::CPass(Some(pass)));
        }
    });
    f!("wgpuComputePassEncoderDispatchWorkgroups", (p: i32, x: i32, y: i32, z: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::CPass(Some(mut pass))) = takeh(p) { pass.dispatch_workgroups(x as u32, y as u32, z as u32); put(Obj::CPass(Some(pass))); }
    });
    f!("wgpuComputePassEncoderSetBindGroup", (p: i32, i: i32, b: i32, _n: i32, _o: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::CPass(Some(mut pass))) = takeh(p) {
            if let Some(bg) = with(|g| g.objs.get(&(b as u32)).map(|o| match o { Obj::Bg(x) => x.clone(), _ => unreachable!() })) { pass.set_bind_group(i as u32, Some(&bg), &[]) }
            put(Obj::CPass(Some(pass)));
        }
    });
    f!("wgpuComputePassEncoderEnd", (p: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        if let Some(Obj::CPass(Some(pass))) = takeh(p) { drop(pass) }
    });
    f!("wgpuComputePassEncoderPushDebugGroup", (_p: i32, _g: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {});
    f!("wgpuComputePassEncoderPopDebugGroup", (_p: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {});

    f!("wgpuCommandEncoderCopyBufferToBuffer", (e: i32, a: i32, ao: i64, b: i32, bo: i64, n: i64), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let Some(Obj::Enc(Some(mut enc))) = takeh(e) else { return };
        with(|g| {
            let (Some(Obj::Buffer(a)), Some(Obj::Buffer(b))) = (g.objs.get(&(a as u32)), g.objs.get(&(b as u32))) else { return };
            enc.copy_buffer_to_buffer(a, ao as u64, b, bo as u64, n as u64);
        });
        with(|g| { g.objs.insert(e as u32, Obj::Enc(Some(enc))); });
    });

    f!("wgpuCommandEncoderCopyTextureToTexture", (e: i32, s: i32, d: i32, ext: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let Some(Obj::Enc(Some(mut enc))) = takeh(e) else { return };
        let src = texel(&m, s as u32);
        let dst = texel(&m, d as u32);
        let size = wgpu::Extent3d { width: m.u32(ext), height: m.u32(ext + 4), depth_or_array_layers: m.u32(ext + 8) };
        with(|g| {
            let (Some(Obj::Texture(a)), Some(Obj::Texture(b))) = (g.objs.get(&src.0), g.objs.get(&dst.0)) else { return };
            enc.copy_texture_to_texture(
                wgpu::TexelCopyTextureInfo { texture: a, mip_level: src.1, origin: src.2, aspect: wgpu::TextureAspect::All },
                wgpu::TexelCopyTextureInfo { texture: b, mip_level: dst.1, origin: dst.2, aspect: wgpu::TextureAspect::All },
                size,
            );
        });
        with(|g| { g.objs.insert(e as u32, Obj::Enc(Some(enc))); });
    });

    f!("wgpuCommandEncoderCopyTextureToBuffer", (e: i32, s: i32, d: i32, ext: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let Some(Obj::Enc(Some(mut enc))) = takeh(e) else { return };
        let src = texel(&m, s as u32);
        let dbuf = m.u32(d + 16);
        let dlay = wgpu::TexelCopyBufferLayout { offset: m.u64(d), bytes_per_row: none_max(m.u32(d + 8)), rows_per_image: none_max(m.u32(d + 12)) };
        let size = wgpu::Extent3d { width: m.u32(ext), height: m.u32(ext + 4), depth_or_array_layers: m.u32(ext + 8) };
        with(|g| {
            let (Some(Obj::Texture(a)), Some(Obj::Buffer(b))) = (g.objs.get(&src.0), g.objs.get(&dbuf)) else { return };
            enc.copy_texture_to_buffer(
                wgpu::TexelCopyTextureInfo { texture: a, mip_level: src.1, origin: src.2, aspect: wgpu::TextureAspect::All },
                wgpu::TexelCopyBufferInfo { buffer: b, layout: dlay }, size,
            );
        });
        with(|g| { g.objs.insert(e as u32, Obj::Enc(Some(enc))); });
    });

    f!("wgpuCommandEncoderResolveQuerySet", (e: i32, q: i32, a: i32, b: i32, d: i32, off: i64), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let Some(Obj::Enc(Some(mut enc))) = takeh(e) else { return };
        with(|g| {
            let (Some(Obj::Query(q)), Some(Obj::Buffer(d))) = (g.objs.get(&(q as u32)), g.objs.get(&(d as u32))) else { return };
            enc.resolve_query_set(q, a as u32..b as u32, d, off as u64);
        });
        with(|g| { g.objs.insert(e as u32, Obj::Enc(Some(enc))); });
    });

    f!("wgpuDeviceCreateQuerySet", (d: i32, desc: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let Some(dev) = g_device() else { return 0 };
        let ty = match m.u32(desc + 12) { 1 => wgpu::QueryType::Occlusion, _ => wgpu::QueryType::Timestamp };
        put(Obj::Query(dev.create_query_set(&wgpu::QuerySetDescriptor { label: None, ty, count: m.u32(desc + 16) }))) as i32
    });

    f!("wgpuCommandEncoderPushDebugGroup", (_e: i32, _g: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {});
    f!("wgpuCommandEncoderPopDebugGroup", (_e: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {});
    f!("wgpuQueueSubmit", (_q: i32, _n: i32, _l: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {});

    f!("wgpuBufferGetSize", (b: i32), i64, |st: &mut wasmtime::Store<St>, m: M<'_>| { with(|g| g.objs.get(&(b as u32)).map(|o| match o { Obj::Buffer(x) => x.size() as i64, _ => 0 })).unwrap_or(0) });
    f!("wgpuTextureGetDepthOrArrayLayers", (t: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| { with(|g| g.objs.get(&(t as u32)).map(|o| match o { Obj::Texture(x) => x.depth_or_array_layers() as i32, _ => 0 })).unwrap_or(0) });
    f!("wgpuTextureGetDimension", (t: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| { with(|g| g.objs.get(&(t as u32)).map(|o| match o { Obj::Texture(x) => match x.dimension() { wgpu::TextureDimension::D1 => 1, wgpu::TextureDimension::D2 => 2, wgpu::TextureDimension::D3 => 3 }, _ => 0 })).unwrap_or(0) });
    f!("wgpuTextureGetUsage", (t: i32), i64, |st: &mut wasmtime::Store<St>, m: M<'_>| { with(|g| g.objs.get(&(t as u32)).map(|o| match o { Obj::Texture(x) => x.usage().bits() as i64, _ => 0 })).unwrap_or(0) });

    f!("wgpuDeviceGetLimits", (d: i32, out: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        let _ = d;
        let l = with(|g| g.device.as_ref().map(|d| d.limits())).unwrap_or_else(wgpu::Limits::downlevel_defaults);
        unsafe {
            let dd = st.data().mem.data();
            let w = |off: u32, v: u32| *(dd[out as usize + off as usize].get() as *mut u32) = v;
            w(4, l.max_texture_dimension_1d); w(8, l.max_texture_dimension_2d); w(12, l.max_texture_dimension_3d);
            w(16, l.max_texture_array_layers); w(20, l.max_bind_groups); w(24, l.max_bindings_per_bind_group);
            w(28, l.max_dynamic_uniform_buffers_per_pipeline_layout); w(32, l.max_dynamic_storage_buffers_per_pipeline_layout);
            w(36, l.max_sampled_textures_per_shader_stage); w(40, l.max_samplers_per_shader_stage);
            w(44, l.max_storage_buffers_per_shader_stage); w(48, l.max_storage_textures_per_shader_stage);
            w(52, l.max_uniform_buffers_per_shader_stage); w(56, l.max_uniform_buffer_binding_size as u32);
            w(60, l.max_storage_buffer_binding_size as u32); w(64, 128); w(68, 16384);
        }
        1
    });

    f!("emwgpuDelete", (h: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| { drop(take(h as u32)) });
    f!("emwgpuDeviceDestroy", (_d: i32), (), |st: &mut wasmtime::Store<St>, m: M<'_>| {});
    f!("emwgpuBufferUnmap", (b: i32), (), |_st: &mut wasmtime::Store<St>, _m: M<'_>| {
        with(|g| {
            if let Some((p, len, write)) = g.maps.remove(&(b as u32)) {
                if let Some(Obj::Buffer(buf)) = g.objs.get(&(b as u32)) {
                    if write {
                        let _ = (p, len);
                    }
                    buf.unmap();
                }
            }
        });
    });

    f!("emwgpuBufferGetConstMappedRange", (b: i32, off: i32, _size: i32), i32, |st: &mut wasmtime::Store<St>, m: M<'_>| {
        with(|g| g.maps.get(&(b as u32)).map(|(p, _, _)| (*p + off as u32) as i32)).unwrap_or(0)
    });

    l.func_wrap_async("env", "emwgpuBufferMapAsync", |c: Caller<'_, St>, (b, fut, off, size, _mode): (i32, i64, i64, i32, i32)| {
        let p = c.data().rt.as_ref().expect("rt").store;
        let futb: Box<dyn std::future::Future<Output = ()> + Send> = Box::new(async move {
            let st = p.get();
            crate::trace::hit("wgpu_map");
            let Some(buf) = with(|g| g.objs.get(&(b as u32)).cloned_buf()) else { return };
            let n = if size == -1 { buf.size() - off as u64 } else { size as u64 };
            let (tx, mut rx) = futures::channel::oneshot::channel();
            buf.slice(off as u64..off as u64 + n).map_async(wgpu::MapMode::Read, move |r| { let _ = tx.send(r); });
            loop {
                with(|g| { if let Some(d) = &g.device { let _ = d.poll(wgpu::PollType::Poll); } });
                match rx.try_recv() {
                    Ok(Some(Ok(()))) => break,
                    Ok(Some(Err(_))) | Err(_) => return,
                    Ok(None) => crate::fibre::sleep(st, std::time::Duration::from_millis(1)).await,
                }
            }
            let view = buf.slice(off as u64..off as u64 + n).get_mapped_range().unwrap();
            let gp = g_malloc(st, n as u32);
            let mem = st.data().mem.clone();
            unsafe { std::ptr::copy_nonoverlapping(view.as_ptr(), mem.data()[gp as usize].get(), n as usize) };
            drop(view);
            with(|g| { g.maps.insert(b as u32, (gp, n as usize, false)); });
            if let Some(inst) = st.data().inst {
                if let Ok(f) = inst.get_typed_func::<(i64, i32, i32), ()>(&mut *st, "emwgpuOnMapAsyncCompleted") {
                    let _ = f.call_async(&mut *st, (fut, 1, 0)).await;
                }
            }
        });
        futb
    })?;

    f!("presentBrowserFrame", (d: i32, tex: i32, cmd: i32), (), |_st: &mut wasmtime::Store<St>, _m: M<'_>| {
        if std::env::var_os("WC_NORENDER").is_some() { return }
        static ONCE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(true);
        if ONCE.swap(false, std::sync::atomic::Ordering::Relaxed) { eprintln!("[wgpu] first present"); }
        let _ = d;
        let Some(Obj::Cmd(cmd)) = take(cmd as u32) else { return };
        let Some(src) = with(|g| g.objs.get(&(tex as u32)).cloned_tex()) else { return };
        let frame = with(|g| {
            let Some(Obj::Surface(s)) = g.objs.get(&0) else { return None };
            match s.get_current_texture() {
                wgpu::CurrentSurfaceTexture::Success(f) | wgpu::CurrentSurfaceTexture::Suboptimal(f) => Some(f),
                _ => None,
            }
        });
        let Some(frame) = frame else { return };
        let Some(dev) = g_device() else { return };
        let mut enc = dev.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        enc.copy_texture_to_texture(
            wgpu::TexelCopyTextureInfo { texture: &src, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::TexelCopyTextureInfo { texture: &frame.texture, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::Extent3d { width: src.width().min(frame.texture.width()), height: src.height().min(frame.texture.height()), depth_or_array_layers: 1 },
        );
        if let Ok(path) = std::env::var("WC_SHOT") {
            static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            if n % 30 == 29 {
                let (w, h) = (frame.texture.width(), frame.texture.height());
                let bpr = (w * 4 + 255) & !255;
                let buf = dev.create_buffer(&wgpu::BufferDescriptor {
                    label: None, size: (bpr * h) as u64,
                    usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ, mapped_at_creation: false,
                });
                let mut enc2 = dev.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
                enc2.copy_texture_to_buffer(
                    wgpu::TexelCopyTextureInfo { texture: &frame.texture, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
                    wgpu::TexelCopyBufferInfo { buffer: &buf, layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(bpr), rows_per_image: None } },
                    wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                );
                let c3 = enc2.finish();
                with(|g| { if let Some(q) = &g.queue { q.submit([c3]); } });
                buf.slice(..).map_async(wgpu::MapMode::Read, |_| {});
                let _ = dev.poll(wgpu::PollType::Wait { submission_index: None, timeout: None });
                if let Ok(view) = buf.slice(..).get_mapped_range() {
                    let mut out = format!("P6\n{w} {h}\n255\n").into_bytes();
                    for y in 0..h {
                        let row = &view[(y * bpr) as usize..];
                        for x in 0..w {
                            let i = (x * 4) as usize;
                            out.push(row[i + 2]); out.push(row[i + 1]); out.push(row[i]);
                        }
                    }
                    drop(view);
                    let _ = std::fs::write(&path, out);
                    eprintln!("[shot] {path} {w}x{h} frame {n}");
                }
                buf.unmap();
            }
        }
        let c2 = enc.finish();
        with(|g| {
            if let Some(q) = &g.queue {
                q.submit([c2, cmd]);
                q.present(frame);
            }
        });
    });

    f!("applyPendingBrowserCanvasResize", (), (), |_st: &mut wasmtime::Store<St>, _m: M<'_>| {
        with(|g| {
            let (sw, sh) = crate::gfx::size();
            if g.size == (sw as u32, sh as u32) { return }
            g.size = (sw as u32, sh as u32);
            let Some(Obj::Surface(s)) = g.objs.get(&0) else { return };
            let Some(dev) = g.device.clone() else { return };
            let Some(ad) = g.adapter.as_ref() else { return };
            let caps = s.get_capabilities(ad);
            let fmt = caps.formats.first().copied().unwrap_or(wgpu::TextureFormat::Bgra8Unorm);
            s.configure(&dev, &wgpu::SurfaceConfiguration {
                color_space: wgpu::SurfaceColorSpace::Srgb,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT, format: fmt,
                width: g.size.0.max(1), height: g.size.1.max(1),
                present_mode: caps.present_modes.first().copied().unwrap_or(wgpu::PresentMode::Fifo),
                alpha_mode: caps.alpha_modes.first().copied().unwrap_or(wgpu::CompositeAlphaMode::Auto),
                view_formats: vec![], desired_maximum_frame_latency: 2,
            });
        })
    });

    l.func_wrap("env", "wgpuDeviceHasFeature", |_c: Caller<'_, St>, _d: i32, f: i32| -> i32 {
        matches!(f, 4 | 5) as i32
    })?;
    Ok(())
}

fn g_device() -> Option<wgpu::Device> {
    with(|g| g.device.clone())
}

fn none_max(v: u32) -> Option<u32> { if v == u32::MAX { None } else { Some(v) } }

fn texel(m: &M, p: u32) -> (u32, u32, wgpu::Origin3d) {
    (m.u32(p), m.u32(p + 4), wgpu::Origin3d { x: m.u32(p + 8), y: m.u32(p + 12), z: m.u32(p + 16) })
}

trait CloneObj4 { fn cloned_bg(self) -> Option<wgpu::BindGroup>; }
impl<'a> CloneObj4 for Option<&'a Obj> { fn cloned_bg(self) -> Option<wgpu::BindGroup> { match self { Some(Obj::Bg(x)) => Some(x.clone()), _ => None } } }

trait CloneObj3 { fn cloned_tex(self) -> Option<wgpu::Texture>; }
impl<'a> CloneObj3 for Option<&'a Obj> { fn cloned_tex(self) -> Option<wgpu::Texture> { match self { Some(Obj::Texture(x)) => Some(x.clone()), _ => None } } }

trait CloneObj2 { fn cloned_pl(self) -> Option<wgpu::PipelineLayout>; fn cloned_shader(self) -> Option<wgpu::ShaderModule>; }
impl<'a> CloneObj2 for Option<&'a Obj> {
    fn cloned_pl(self) -> Option<wgpu::PipelineLayout> { match self { Some(Obj::Pl(x)) => Some(x.clone()), _ => None } }
    fn cloned_shader(self) -> Option<wgpu::ShaderModule> { match self { Some(Obj::Shader(x)) => Some(x.clone()), _ => None } }
}

trait CloneObj { fn cloned_bgl(self) -> Option<wgpu::BindGroupLayout>; fn cloned_buf(self) -> Option<wgpu::Buffer>; fn cloned_sampler(self) -> Option<wgpu::Sampler>; fn cloned_view(self) -> Option<wgpu::TextureView>; }
impl<'a> CloneObj for Option<&'a Obj> {
    fn cloned_bgl(self) -> Option<wgpu::BindGroupLayout> { match self { Some(Obj::Bgl(x)) => Some(x.clone()), _ => None } }
    fn cloned_buf(self) -> Option<wgpu::Buffer> { match self { Some(Obj::Buffer(x)) => Some(x.clone()), _ => None } }
    fn cloned_sampler(self) -> Option<wgpu::Sampler> { match self { Some(Obj::Sampler(x)) => Some(x.clone()), _ => None } }
    fn cloned_view(self) -> Option<wgpu::TextureView> { match self { Some(Obj::View(x)) => Some(x.clone()), _ => None } }
}

trait CloneRp {
    fn cloned_rp(self) -> Option<wgpu::RenderPipeline>;
}
impl<'a> CloneRp for Option<&'a Obj> {
    fn cloned_rp(self) -> Option<wgpu::RenderPipeline> {
        match self { Some(Obj::Rp(p)) => Some(p.clone()), _ => None }
    }
}
