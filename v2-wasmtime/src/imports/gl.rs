//! WebGL2-shaped imports backed by real GLES3.
//!
//! Guest linear memory is a host mapping, so ordinary pointers are valid host
//! pointers and are passed straight through.  Only mapped buffers and sync
//! objects need a handle translation; `glGetString` copies into guest memory.
use std::collections::HashMap;
use std::sync::Mutex;

use wasmtime::{AsContextMut, Caller, FuncType, Linker, Val, ValType};

use crate::state::St;

const I: ValType = ValType::I32;
const L: ValType = ValType::I64;
const V: ValType = ValType::F32;

// name, params, result
macro_rules! table {
    ($($name:literal, $p:expr, $r:expr;)*) => {
        pub const TABLE: &[(&str, &[ValType], Option<ValType>)] = &[
            $(($name, $p, $r),)*
        ];
    };
}

table! {
    "glScissor", &[I, I, I, I], None;
    "glInvalidateFramebuffer", &[I, I, I], None;
    "glBindFramebuffer", &[I, I], None;
    "glActiveTexture", &[I], None;
    "glBindTexture", &[I, I], None;
    "glCopyTexSubImage2D", &[I, I, I, I, I, I, I, I], None;
    "glBindBufferRange", &[I, I, I, I, I], None;
    "glBlitFramebuffer", &[I, I, I, I, I, I, I, I, I, I], None;
    "glDisable", &[I], None;
    "glEnable", &[I], None;
    "glCullFace", &[I], None;
    "glPolygonOffset", &[V, V], None;
    "glColorMask", &[I, I, I, I], None;
    "glBlendFunc", &[I, I], None;
    "glBlendFuncSeparate", &[I, I, I, I], None;
    "glDepthFunc", &[I], None;
    "glDepthMask", &[I], None;
    "glStencilFunc", &[I, I, I], None;
    "glStencilOp", &[I, I, I], None;
    "glUseProgram", &[I], None;
    "glStencilMask", &[I], None;
    "glClearBufferfv", &[I, I, I], None;
    "glClearBufferfi", &[I, I, V, I], None;
    "glClearBufferiv", &[I, I, I], None;
    "glClearColor", &[V, V, V, V], None;
    "glClearDepthf", &[V], None;
    "glClearStencil", &[I], None;
    "glClear", &[I], None;
    "glBindBufferBase", &[I, I, I], None;
    "glViewport", &[I, I, I, I], None;
    "glDrawBuffers", &[I, I], None;
    "glGenerateMipmap", &[I], None;
    "glGenFramebuffers", &[I, I], None;
    "glFramebufferTextureLayer", &[I, I, I, I, I], None;
    "glCheckFramebufferStatus", &[I], Some(I);
    "glDeleteFramebuffers", &[I, I], None;
    "glDeleteBuffers", &[I, I], None;
    "glDeleteShader", &[I], None;
    "glDeleteProgram", &[I], None;
    "glDeleteTextures", &[I, I], None;
    "glDeleteRenderbuffers", &[I, I], None;
    "glGetProgramiv", &[I, I, I], None;
    "glBindBuffer", &[I, I], None;
    "glMapBufferRange", &[I, I, I, I], Some(I);
    "glGetError", &[], Some(I);
    "glGetString", &[I], Some(I);
    "glGetIntegerv", &[I, I], None;
    "glGenVertexArrays", &[I, I], None;
    "glBindVertexArray", &[I], None;
    "glDeleteVertexArrays", &[I, I], None;
    "glUnmapBuffer", &[I], Some(I);
    "glBufferSubData", &[I, I, I, I], None;
    "glGenBuffers", &[I, I], None;
    "glBufferData", &[I, I, I, I], None;
    "glClientWaitSync", &[I, I, L], Some(I);
    "glDeleteSync", &[I], None;
    "glFenceSync", &[I, I], Some(I);
    "glFramebufferTexture2D", &[I, I, I, I, I], None;
    "glFramebufferRenderbuffer", &[I, I, I, I], None;
    "glEnableVertexAttribArray", &[I], None;
    "glVertexAttribPointer", &[I, I, I, I, I, I], None;
    "glDisableVertexAttribArray", &[I], None;
    "glDrawElements", &[I, I, I, I], None;
    "glDrawArrays", &[I, I, I], None;
    "glDrawElementsInstanced", &[I, I, I, I, I], None;
    "glDrawArraysInstanced", &[I, I, I, I], None;
    "glGetUniformLocation", &[I, I], Some(I);
    "glCreateProgram", &[], Some(I);
    "glAttachShader", &[I, I], None;
    "glBindAttribLocation", &[I, I, I], None;
    "glLinkProgram", &[I], None;
    "glGetProgramInfoLog", &[I, I, I, I], None;
    "glUniform1i", &[I, I], None;
    "glGetUniformBlockIndex", &[I, I], Some(I);
    "glUniformBlockBinding", &[I, I, I], None;
    "glGetActiveUniformBlockiv", &[I, I, I, I], None;
    "glGetActiveUniform", &[I, I, I, I, I, I, I], None;
    "glCreateShader", &[I], Some(I);
    "glShaderSource", &[I, I, I, I], None;
    "glCompileShader", &[I], None;
    "glGetShaderiv", &[I, I, I], None;
    "glGetShaderInfoLog", &[I, I, I, I], None;
    "glGenTextures", &[I, I], None;
    "glTexStorage3D", &[I, I, I, I, I, I], None;
    "glTexStorage2D", &[I, I, I, I, I], None;
    "glCompressedTexImage3D", &[I, I, I, I, I, I, I, I, I], None;
    "glCompressedTexImage2D", &[I, I, I, I, I, I, I, I], None;
    "glTexImage3D", &[I, I, I, I, I, I, I, I, I, I], None;
    "glTexImage2D", &[I, I, I, I, I, I, I, I, I], None;
    "glTexParameteri", &[I, I, I], None;
    "glPixelStorei", &[I, I], None;
    "glReadPixels", &[I, I, I, I, I, I, I], None;
    "glGenRenderbuffers", &[I, I], None;
    "glBindRenderbuffer", &[I, I], None;
    "glRenderbufferStorageMultisample", &[I, I, I, I, I], None;
    "glRenderbufferStorage", &[I, I, I, I], None;
    "glCompressedTexSubImage3D", &[I, I, I, I, I, I, I, I, I, I, I], None;
    "glCompressedTexSubImage2D", &[I, I, I, I, I, I, I, I, I], None;
    "glTexSubImage3D", &[I, I, I, I, I, I, I, I, I, I, I], None;
    "glTexSubImage2D", &[I, I, I, I, I, I, I, I, I], None;
    "glTexParameterfv", &[I, I, I], None;
    "glTexParameterf", &[I, I, V], None;
    "glClipControlEXT", &[I, I], None;
}

static SYNCS: Mutex<Option<HashMap<u32, u64>>> = Mutex::new(None);

fn syncs() -> std::sync::MutexGuard<'static, Option<HashMap<u32, u64>>> {
    SYNCS.lock().unwrap()
}

fn get_string(c: &mut Caller<'_, St>, name: u32) -> i32 {
    let mut sctx = c.as_context_mut();
    let st = &mut sctx;
    let host = crate::gfx::call("glGetString", &[name as u64], &mut sctx);
    let text = if name == 0x1f03 {
        // GLES3 core only exposes the list through glGetStringi
        let mut out = Vec::new();
        let mut count = 0i32;
        // Host-side stack pointers: call_raw (no guest-offset translation).
        let _ = crate::gfx::call_raw("glGetIntegerv", &[0x821d, &mut count as *mut i32 as u64], &mut sctx);
        for i in 0..count.max(0) as u32 {
            let p = crate::gfx::call_raw("glGetStringi", &[0x1f03, i as u64], &mut sctx);
            if p != 0 {
                let s = unsafe { std::ffi::CStr::from_ptr(p as *const _) }.to_string_lossy();
                out.extend_from_slice(s.as_bytes());
                out.push(b' ');
            }
        }
        String::from_utf8_lossy(&out).into_owned()
    } else if host == 0 {
        String::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(host as *const _) }.to_string_lossy().into_owned()
    };
    crate::gfx::str_to_guest(&mut sctx, &text) as i32
}

pub fn register(l: &mut Linker<St>) -> wasmtime::Result<()> {
    let engine = l.engine().clone();

    // One real EGL/GLES3 context, behind Emscripten's single-context model.
    // The handle is 1 (the glue's `f.contexts` id), never a fake cap.
    l.func_wrap("env", "emscripten_webgl_create_context", |mut c: Caller<'_, St>, _t: i32, attrs: i32| -> i32 {
        crate::trace::hit("webgl_create_context");
        if let Err(e) = crate::gfx::ensure() {
            eprintln!("[gl ] egl context: {e}");
            return 0;
        }
        // WebClient.js::_s rejects explicitSwapControl (attributes byte 25);
        // honour that decision instead of returning a context the engine
        // cannot present through.
        if attrs != 0 {
            let d = c.data().mem.data();
            if let Some(p) = d.get(attrs as usize + 25) {
                if unsafe { *p.get() } != 0 { return 0 }
            }
        }
        c.data_mut().gl_ctx = true;
        1
    })?;
    l.func_wrap("env", "emscripten_webgl_make_context_current", |_c: Caller<'_, St>, h: i32| -> i32 {
        if h == 0 || h == 1 { crate::gfx::make_current(); 0 } else { -5 }
    })?;
    l.func_wrap("env", "emscripten_webgl_destroy_context", |mut c: Caller<'_, St>, h: i32| -> i32 {
        if h != 0 && h != 1 { return 0 }
        c.data_mut().gl_ctx = false;
        0
    })?;
    l.func_wrap("env", "emscripten_webgl_get_context_attributes",
        |mut c: Caller<'_, St>, h: i32, out: i32| -> i32 {
            if out == 0 { return -5 }
            if h != 1 { return -3 }
            // Report the real EGL config, not the shape-only Deno values.
            let alpha = crate::gfx::gl_int(0x0d55) > 0;    // GL_ALPHA_BITS
            let depth = crate::gfx::gl_int(0x0d56) > 0;    // GL_DEPTH_BITS
            let stencil = crate::gfx::gl_int(0x0d57) > 0;  // GL_STENCIL_BITS
            let samples = crate::gfx::gl_int(0x80a9);      // GL_SAMPLES
            let d = c.data().mem.data();
            let b = out as usize;
            let w8 = |off: usize, v: u8| {
                if b + off < d.len() { unsafe { *d[b + off].get() = v } }
            };
            let w32 = |off: usize, v: u32| {
                if b + off + 4 <= d.len() {
                    unsafe { *(d[b + off].get() as *mut u32) = v }
                }
            };
            w8(0, alpha as u8);
            w8(1, depth as u8);
            w8(2, stencil as u8);
            w8(3, (samples > 1) as u8);
            w8(4, 1); // premultipliedAlpha
            w8(5, 0); // preserveDrawingBuffer
            w32(8, 0); // powerPreference: default
            w8(12, 0); // failIfMajorPerformanceCaveat
            w32(16, 3); // majorVersion: GLES3 (WebGL2-compatible)
            w32(20, 0); // minorVersion
            w8(24, 1); // enableExtensionsByDefault
            w8(33, 0); // desynchronized
            0
        })?;
    static LAST_SIZE: std::sync::atomic::AtomicU64 =
        std::sync::atomic::AtomicU64::new(u64::MAX);
    l.func_wrap("env", "applyPendingCanvasResize", |mut c: Caller<'_, St>, w: i32, h: i32| -> i32 {
        let (gw, gh) = crate::gfx::size();
        let cur = ((gw as u64) << 32) | gh as u64;
        let last = LAST_SIZE.load(std::sync::atomic::Ordering::Relaxed);
        if last != u64::MAX && last == cur {
            return 0;
        }
        // First poll (or a real resize) reports the size once, as the glue's
        // `applyPendingCanvasResize` does.
        LAST_SIZE.store(cur, std::sync::atomic::Ordering::Relaxed);
        let d = c.data().mem.data();
        if w != 0 { unsafe { *(d[w as usize].get() as *mut u32) = gw as u32 } }
        if h != 0 { unsafe { *(d[h as usize].get() as *mut u32) = gh as u32 } }
        1
    })?;

    for &(name, params, ref result) in TABLE {
        let ret = if result.is_some() { Some(I) } else { None };
        let ty = FuncType::new(&engine, params.iter().cloned(), ret.into_iter());
        let name_s = name.to_string();
        l.func_new("env", name, ty, move |mut c: Caller<'_, St>, args: &[Val], out: &mut [Val]| {
            crate::trace::hit(name);
            let mut bits = [0u64; 12];
            for (i, a) in args.iter().enumerate().take(12) {
                bits[i] = match a {
                    Val::I32(v) => *v as u32 as u64,
                    Val::I64(v) => *v as u64,
                    Val::F32(v) => *v as u64,
                    _ => 0,
                };
            }
            let mut sctx = c.as_context_mut();
            let st = &mut sctx;
            let r = match name_s.as_str() {
                "glGetString" => get_string(&mut c, args[0].unwrap_i32() as u32) as u64,
                "glFenceSync" => {
                    let h = crate::gfx::call(&name_s, &bits[..2], st);
                    let mut map = syncs();
                    let map = map.get_or_insert_with(Default::default);
                    let id = map.len() as u32 + 1;
                    map.insert(id, h);
                    id as u64
                }
                "glClientWaitSync" => {
                    let id = args[0].unwrap_i32() as u32;
                    let h = syncs().as_ref().and_then(|m| m.get(&id).copied()).unwrap_or(0);
                    bits[0] = h;
                    crate::gfx::call(&name_s, &bits[..3], st)
                }
                "glDeleteSync" => {
                    let id = args[0].unwrap_i32() as u32;
                    let h = syncs().as_mut().and_then(|m| m.remove(&id)).unwrap_or(0);
                    crate::gfx::call(&name_s, &[h], st)
                }
                _ => crate::gfx::call(&name_s, &bits[..args.len()], st),
            };
            if let Some(o) = out.first_mut() {
                *o = Val::I32(r as u32 as i32);
            }
            Ok(())
        })?;
    }

    // WebGPU stays absent: the surface asks for webgl2 through `getContext`.
    Ok(())
}
