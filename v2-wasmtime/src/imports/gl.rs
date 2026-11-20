//! A WebGL2-shaped stub surface.
//!
//! The engine only needs a context that reports plausible limits and shaders
//! that say they compiled; nothing is really rasterised.  Keeping the shape
//! (string pointers, non-zero object ids, success statuses) lets the surface,
//! the render job and with them the data model step keep running.
use std::sync::Mutex;

use wasmtime::{Caller, Linker};

use crate::state::{wbytes, St};

struct Gl {
    ids: i32,
    string: u32,
    mapped: u32,
}

static GL: Mutex<Gl> = Mutex::new(Gl { ids: 1, string: 0, mapped: 0 });

fn id() -> i32 {
    let mut g = GL.lock().unwrap();
    g.ids += 1;
    g.ids
}

/// Guest buffer holding the string last returned by `glGetString`.
fn string(c: &mut Caller<'_, St>, s: &str) -> i32 {
    let mut g = GL.lock().unwrap();
    if g.string == 0 {
        g.string = 0x10_0000; // below the module's static image end
    }
    let p = g.string;
    drop(g);
    wbytes(c, p, s.as_bytes());
    wbytes(c, p + s.len() as u32, &[0]);
    p as i32
}

fn put(c: &mut Caller<'_, St>, p: i32, v: i32) {
    wbytes(c, p as u32, &v.to_le_bytes());
}

const GL_VERSION: i32 = 0x1F02;
const GL_VENDOR: i32 = 0x1F00;
const GL_RENDERER: i32 = 0x1F01;
const GL_SHADING_LANGUAGE_VERSION: i32 = 0x8B8C;
const GL_EXTENSIONS: i32 = 0x1F03;

pub fn register(l: &mut Linker<St>) -> wasmtime::Result<()> {
    macro_rules! v {
        ($name:literal, $($a:ident : $t:ty),*) => {
            l.func_wrap("env", $name, |_c: Caller<'_, St>, $($a: $t),*| { crate::trace::hit($name); })?;
        };
    }
    macro_rules! r {
        ($name:literal, $($a:ident : $t:ty),* => $v:expr) => {
            l.func_wrap("env", $name, |_c: Caller<'_, St>, $($a: $t),*| -> i32 { crate::trace::hit($name); $v })?;
        };
    }

    // context
    r!("emscripten_webgl_create_context", _a: i32, _b: i32 => 1);
    r!("emscripten_webgl_make_context_current", _a: i32 => 0);
    r!("emscripten_webgl_destroy_context", _a: i32 => 0);
    r!("emscripten_webgl_get_context_attributes", _a: i32, _b: i32 => 0);
    r!("applyPendingCanvasResize", _a: i32, _b: i32 => 0);

    // strings and limits
    l.func_wrap("env", "glGetString", |mut c: Caller<'_, St>, name: i32| -> i32 {
        crate::trace::hit("glGetString");
        let s = match name {
            GL_VENDOR => "Roblox",
            GL_RENDERER => "alcohol",
            GL_VERSION => "WebGL 2.0 (OpenGL ES 3.0 Chromium)",
            GL_SHADING_LANGUAGE_VERSION => "WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0 Chromium)",
            GL_EXTENSIONS => {
                "GL_EXT_texture_compression_s3tc GL_EXT_texture_filter_anisotropic \
                 GL_EXT_color_buffer_float GL_OES_texture_float_linear"
            }
            _ => "",
        };
        string(&mut c, s)
    })?;
    l.func_wrap("env", "glGetIntegerv", |mut c: Caller<'_, St>, pname: i32, out: i32| {
        crate::trace::hit("glGetIntegerv");
        let v = match pname {
            0x0D33 => 8192,   // MAX_TEXTURE_SIZE
            0x851C => 8192,   // MAX_CUBE_MAP_TEXTURE_SIZE
            0x8073 => 2048,   // MAX_3D_TEXTURE_SIZE
            0x88FF => 2048,   // MAX_ARRAY_TEXTURE_LAYERS
            0x84E8 => 8192,   // MAX_RENDERBUFFER_SIZE
            0x8869 => 16,     // MAX_VERTEX_ATTRIBS
            0x8B4D => 16,     // MAX_COMBINED_TEXTURE_IMAGE_UNITS
            0x8872 => 16,     // MAX_TEXTURE_IMAGE_UNITS
            0x8B4C => 16,     // MAX_VERTEX_TEXTURE_IMAGE_UNITS
            0x8A2F => 24,     // MAX_UNIFORM_BUFFER_BINDINGS
            0x8A30 => 65536,  // MAX_UNIFORM_BLOCK_SIZE
            0x8A34 => 256,    // UNIFORM_BUFFER_OFFSET_ALIGNMENT
            0x8CDF => 8,      // MAX_COLOR_ATTACHMENTS
            0x8824 => 8,      // MAX_DRAW_BUFFERS
            0x8D57 => 4,      // MAX_SAMPLES
            0x8B4B => 16,     // MAX_VERTEX_UNIFORM_VECTORS
            0x8B49 => 1024,   // MAX_FRAGMENT_UNIFORM_VECTORS
            0x821D => 4,      // NUM_EXTENSIONS
            0x0D3A => {       // MAX_VIEWPORT_DIMS
                put(&mut c, out, 8192);
                put(&mut c, out + 4, 8192);
                return;
            }
            0x0BA2 => {       // VIEWPORT
                for i in 0..4 {
                    put(&mut c, out + i * 4, 0);
                }
                return;
            }
            _ => 0,
        };
        put(&mut c, out, v);
    })?;
    r!("glGetError", => 0);
    r!("glCheckFramebufferStatus", _a: i32 => 0x8CD5);

    // objects
    r!("glCreateShader", _a: i32 => id());
    r!("glCreateProgram", => id());
    r!("glGetUniformLocation", _p: i32, _n: i32 => id());
    r!("glGetAttribLocation", _p: i32, _n: i32 => (id() % 16));
    r!("glGetUniformBlockIndex", _p: i32, _n: i32 => (id() % 8));
    r!("glFenceSync", _a: i32, _b: i32 => id());
    r!("glClientWaitSync", _a: i32, _b: i32, _t: i64 => 0x911A);
    r!("glMapBufferRange", _t: i32, _o: i32, len: i32, _f: i32 => { let _ = len; 0 });
    r!("glUnmapBuffer", _t: i32 => 1);

    l.func_wrap("env", "glGetShaderiv", |mut c: Caller<'_, St>, _s: i32, pname: i32, out: i32| {
        crate::trace::hit("glGetShaderiv");
        put(&mut c, out, if pname == 0x8B81 { 1 } else { 0 });
    })?;
    l.func_wrap("env", "glGetProgramiv", |mut c: Caller<'_, St>, _s: i32, pname: i32, out: i32| {
        crate::trace::hit("glGetProgramiv");
        put(&mut c, out, if pname == 0x8B82 { 1 } else { 0 });
    })?;
    l.func_wrap("env", "glGetShaderInfoLog", |mut c: Caller<'_, St>, _s: i32,
     _n: i32, len: i32, out: i32| {
        crate::trace::hit("glGetShaderInfoLog");
        if len != 0 { put(&mut c, len, 0) }
        let _ = out;
    })?;
    l.func_wrap("env", "glGetProgramInfoLog", |mut c: Caller<'_, St>, _s: i32,
     _n: i32, len: i32, _o: i32| {
        crate::trace::hit("glGetProgramInfoLog");
        if len != 0 { put(&mut c, len, 0) }
    })?;
    l.func_wrap("env", "glGetActiveUniform", |mut c: Caller<'_, St>, _p: i32, _i: i32,
     _b: i32, len: i32, size: i32, ty: i32, _n: i32| {
        crate::trace::hit("glGetActiveUniform");
        put(&mut c, len, 0);
        put(&mut c, size, 0);
        put(&mut c, ty, 0);
    })?;
    l.func_wrap("env", "glGetActiveUniformBlockiv", |mut c: Caller<'_, St>,
     _p: i32, _i: i32, _n: i32, out: i32| {
        crate::trace::hit("glGetActiveUniformBlockiv");
        put(&mut c, out, 0);
    })?;
    l.func_wrap("env", "glGenBuffers", |mut c: Caller<'_, St>, n: i32, out: i32| {
        crate::trace::hit("glGenBuffers");
        for i in 0..n.max(0) { put(&mut c, out + i * 4, id()); }
    })?;
    l.func_wrap("env", "glGenTextures", |mut c: Caller<'_, St>, n: i32, out: i32| {
        crate::trace::hit("glGenTextures");
        for i in 0..n.max(0) { put(&mut c, out + i * 4, id()); }
    })?;
    l.func_wrap("env", "glGenVertexArrays", |mut c: Caller<'_, St>, n: i32, out: i32| {
        crate::trace::hit("glGenVertexArrays");
        for i in 0..n.max(0) { put(&mut c, out + i * 4, id()); }
    })?;
    l.func_wrap("env", "glGenFramebuffers", |mut c: Caller<'_, St>, n: i32, out: i32| {
        crate::trace::hit("glGenFramebuffers");
        for i in 0..n.max(0) { put(&mut c, out + i * 4, id()); }
    })?;
    l.func_wrap("env", "glGenRenderbuffers", |mut c: Caller<'_, St>, n: i32, out: i32| {
        crate::trace::hit("glGenRenderbuffers");
        for i in 0..n.max(0) { put(&mut c, out + i * 4, id()); }
    })?;

    // everything else: note it and ignore
    v!("glActiveTexture", _a: i32);
    v!("glAttachShader", _a: i32, _b: i32);
    v!("glBindAttribLocation", _a: i32, _b: i32, _c: i32);
    v!("glBindBuffer", _a: i32, _b: i32);
    v!("glBindBufferBase", _a: i32, _b: i32, _c: i32);
    v!("glBindBufferRange", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32);
    v!("glBindFramebuffer", _a: i32, _b: i32);
    v!("glBindRenderbuffer", _a: i32, _b: i32);
    v!("glBindTexture", _a: i32, _b: i32);
    v!("glBindVertexArray", _a: i32);
    v!("glBlendFunc", _a: i32, _b: i32);
    v!("glBlendFuncSeparate", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glBlitFramebuffer", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32, _i: i32, _j: i32);
    v!("glBufferData", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glBufferSubData", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glClear", _a: i32);
    v!("glClearBufferfi", _a: i32, _b: i32, _c: f32, _d: i32);
    v!("glClearBufferfv", _a: i32, _b: i32, _c: i32);
    v!("glClearBufferiv", _a: i32, _b: i32, _c: i32);
    v!("glClearColor", _a: f32, _b: f32, _c: f32, _d: f32);
    v!("glClearDepthf", _a: f32);
    v!("glClearStencil", _a: i32);
    v!("glClipControlEXT", _a: i32, _b: i32);
    v!("glColorMask", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glCompileShader", _a: i32);
    v!("glCompressedTexImage2D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32);
    v!("glCompressedTexImage3D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32, _i: i32);
    v!("glCompressedTexSubImage2D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32, _i: i32);
    v!("glCompressedTexSubImage3D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32, _i: i32, _j: i32, _k: i32);
    v!("glCopyTexSubImage2D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32);
    v!("glCullFace", _a: i32);
    v!("glDeleteBuffers", _a: i32, _b: i32);
    v!("glDeleteFramebuffers", _a: i32, _b: i32);
    v!("glDeleteProgram", _a: i32);
    v!("glDeleteRenderbuffers", _a: i32, _b: i32);
    v!("glDeleteShader", _a: i32);
    v!("glDeleteSync", _a: i32);
    v!("glDeleteTextures", _a: i32, _b: i32);
    v!("glDeleteVertexArrays", _a: i32, _b: i32);
    v!("glDepthFunc", _a: i32);
    v!("glDepthMask", _a: i32);
    v!("glDisable", _a: i32);
    v!("glDisableVertexAttribArray", _a: i32);
    v!("glDrawArrays", _a: i32, _b: i32, _c: i32);
    v!("glDrawArraysInstanced", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glDrawBuffers", _a: i32, _b: i32);
    v!("glDrawElements", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glDrawElementsInstanced", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32);
    v!("glEnable", _a: i32);
    v!("glEnableVertexAttribArray", _a: i32);
    v!("glFramebufferRenderbuffer", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glFramebufferTexture2D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32);
    v!("glFramebufferTextureLayer", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32);
    v!("glGenerateMipmap", _a: i32);
    v!("glInvalidateFramebuffer", _a: i32, _b: i32, _c: i32);
    v!("glLinkProgram", _a: i32);
    v!("glPixelStorei", _a: i32, _b: i32);
    v!("glPolygonOffset", _a: f32, _b: f32);
    v!("glReadPixels", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32);
    v!("glRenderbufferStorage", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glRenderbufferStorageMultisample", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32);
    v!("glScissor", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glShaderSource", _a: i32, _b: i32, _c: i32, _d: i32);
    v!("glStencilFunc", _a: i32, _b: i32, _c: i32);
    v!("glStencilMask", _a: i32);
    v!("glStencilOp", _a: i32, _b: i32, _c: i32);
    v!("glTexImage2D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32, _i: i32);
    v!("glTexImage3D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32, _i: i32, _j: i32);
    v!("glTexParameterf", _a: i32, _b: i32, _c: f32);
    v!("glTexParameterfv", _a: i32, _b: i32, _c: i32);
    v!("glTexParameteri", _a: i32, _b: i32, _c: i32);
    v!("glTexStorage2D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32);
    v!("glTexStorage3D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32);
    v!("glTexSubImage2D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32, _i: i32);
    v!("glTexSubImage3D", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32, _g: i32, _h: i32, _i: i32, _j: i32, _k: i32);
    v!("glUniform1i", _a: i32, _b: i32);
    v!("glUniformBlockBinding", _a: i32, _b: i32, _c: i32);
    v!("glUseProgram", _a: i32);
    v!("glVertexAttribPointer", _a: i32, _b: i32, _c: i32, _d: i32, _e: i32, _f: i32);
    v!("glViewport", _a: i32, _b: i32, _c: i32, _d: i32);
    Ok(())
}
