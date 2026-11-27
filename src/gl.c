// GL ES / WebGL imports from WC.h.  Names and signatures are taken verbatim
// from the generated header; no import indices are used.  Pointer arguments
// are guest linear-memory addresses and are always resolved through guest.h.
#include "host.h"
#include "guest.h"
#include "gl.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One entry per GLES function the engine imports.  GLAD fills these when it
 * is compiled in; otherwise SDL_GL_GetProcAddress fills them at gl_init(). */
#define GL_FN_LIST(X) \
  X(ActiveTexture) X(AttachShader) X(BindAttribLocation) X(BindBuffer) \
  X(BindBufferBase) X(BindBufferRange) X(BindFramebuffer) X(BindRenderbuffer) \
  X(BindTexture) X(BindVertexArray) X(BlendFunc) X(BlendFuncSeparate) \
  X(BlitFramebuffer) X(BufferData) X(BufferSubData) X(CheckFramebufferStatus) \
  X(Clear) X(ClearBufferfi) X(ClearBufferfv) X(ClearBufferiv) X(ClearColor) \
  X(ClearDepthf) X(ClearStencil) X(ClientWaitSync) X(ClipControlEXT) \
  X(ColorMask) X(CompileShader) X(CompressedTexImage2D) X(CompressedTexImage3D) \
  X(CompressedTexSubImage2D) X(CompressedTexSubImage3D) X(CopyTexSubImage2D) \
  X(CreateProgram) X(CreateShader) X(CullFace) X(DeleteBuffers) \
  X(DeleteFramebuffers) X(DeleteProgram) X(DeleteRenderbuffers) X(DeleteShader) \
  X(DeleteSync) X(DeleteTextures) X(DeleteVertexArrays) X(DepthFunc) \
  X(DepthMask) X(Disable) X(DisableVertexAttribArray) X(DrawArrays) \
  X(DrawArraysInstanced) X(DrawBuffers) X(DrawElements) X(DrawElementsInstanced) \
  X(Enable) X(EnableVertexAttribArray) X(FenceSync) X(Flush) X(FramebufferRenderbuffer) \
  X(FramebufferTexture2D) X(FramebufferTextureLayer) X(GenBuffers) \
  X(GenFramebuffers) X(GenRenderbuffers) X(GenTextures) X(GenVertexArrays) \
  X(GenerateMipmap) X(GetActiveUniform) X(GetActiveUniformBlockiv) X(GetError) \
  X(GetIntegerv) X(GetProgramInfoLog) X(GetProgramiv) X(GetShaderInfoLog) \
  X(GetShaderiv) X(GetString) X(GetUniformBlockIndex) X(GetUniformLocation) \
  X(InvalidateFramebuffer) X(LinkProgram) X(MapBufferRange) X(PixelStorei) \
  X(PolygonOffset) X(ReadPixels) X(RenderbufferStorage) \
  X(RenderbufferStorageMultisample) X(Scissor) X(ShaderSource) X(StencilFunc) \
  X(StencilMask) X(StencilOp) X(TexImage2D) X(TexImage3D) X(TexParameterf) \
  X(TexParameterfv) X(TexParameteri) X(TexStorage2D) X(TexStorage3D) \
  X(TexSubImage2D) X(TexSubImage3D) X(Uniform1i) X(UniformBlockBinding) \
  X(UnmapBuffer) X(UseProgram) X(VertexAttribPointer) X(Viewport)

/* All core GLES functions GLAD resolves; extension entry points come from the
 * raw SDL proc loader instead. */
#define GL_GLAD_LIST(X) \
  X(ActiveTexture) X(AttachShader) X(BindAttribLocation) X(BindBuffer) \
  X(BindBufferBase) X(BindBufferRange) X(BindFramebuffer) X(BindRenderbuffer) \
  X(BindTexture) X(BindVertexArray) X(BlendFunc) X(BlendFuncSeparate) \
  X(BlitFramebuffer) X(BufferData) X(BufferSubData) X(CheckFramebufferStatus) \
  X(Clear) X(ClearBufferfi) X(ClearBufferfv) X(ClearBufferiv) X(ClearColor) \
  X(ClearDepthf) X(ClearStencil) X(ClientWaitSync) X(ColorMask) X(CompileShader) \
  X(CompressedTexImage2D) X(CompressedTexImage3D) X(CompressedTexSubImage2D) \
  X(CompressedTexSubImage3D) X(CopyTexSubImage2D) X(CreateProgram) \
  X(CreateShader) X(CullFace) X(DeleteBuffers) X(DeleteFramebuffers) \
  X(DeleteProgram) X(DeleteRenderbuffers) X(DeleteShader) X(DeleteSync) \
  X(DeleteTextures) X(DeleteVertexArrays) X(DepthFunc) X(DepthMask) X(Disable) \
  X(DisableVertexAttribArray) X(DrawArrays) X(DrawArraysInstanced) X(DrawBuffers) \
  X(DrawElements) X(DrawElementsInstanced) X(Enable) X(EnableVertexAttribArray) \
  X(FenceSync) X(Flush) X(FramebufferRenderbuffer) X(FramebufferTexture2D) \
  X(FramebufferTextureLayer) X(GenBuffers) X(GenFramebuffers) \
  X(GenRenderbuffers) X(GenTextures) X(GenVertexArrays) X(GenerateMipmap) \
  X(GetActiveUniform) X(GetActiveUniformBlockiv) X(GetError) X(GetIntegerv) \
  X(GetProgramInfoLog) X(GetProgramiv) X(GetShaderInfoLog) X(GetShaderiv) \
  X(GetString) X(GetUniformBlockIndex) X(GetUniformLocation) \
  X(InvalidateFramebuffer) X(LinkProgram) X(MapBufferRange) X(PixelStorei) \
  X(PolygonOffset) X(ReadPixels) X(RenderbufferStorage) \
  X(RenderbufferStorageMultisample) X(Scissor) X(ShaderSource) X(StencilFunc) \
  X(StencilMask) X(StencilOp) X(TexImage2D) X(TexImage3D) X(TexParameterf) \
  X(TexParameterfv) X(TexParameteri) X(TexStorage2D) X(TexStorage3D) \
  X(TexSubImage2D) X(TexSubImage3D) X(Uniform1i) X(UniformBlockBinding) \
  X(UnmapBuffer) X(UseProgram) X(VertexAttribPointer) X(Viewport)

#define GL_DECLARE(name) static void* p_gl_##name;
GL_FN_LIST(GL_DECLARE)

static int g_ready;
static void* g_map_host;
static u32 g_map_guest, g_map_len;
static u32 g_str_keys[16], g_str_vals[16], g_str_n;

#define GL_P(name) (p_gl_##name)
#define GL_V(name, ...) ((void (*)(__VA_ARGS__))GL_P(name))
#define GL_R(name, ret, ...) ((ret (*)(__VA_ARGS__))GL_P(name))

static void gl_miss(const char* name) {
  static unsigned char seen[256];
  unsigned h = 2166136261u;
  for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
    h = (h ^ *p) * 16777619u;
  unsigned slot = h & 255u;
  if (!seen[slot]) {
    seen[slot] = 1;
    fprintf(stderr, "[gl] %s unavailable\n", name);
  }
}

static void gl_badptr(void) {
  static int once;
  if (!once++) fprintf(stderr, "[gl] invalid guest pointer\n");
}

static u32 gl_guest_alloc(u32 n) {
  return weblox_instance ? w2c_WebClient_malloc(weblox_instance, n) : 0;
}

static void gl_guest_free(u32 p) {
  if (weblox_instance && p) w2c_WebClient_free(weblox_instance, p);
}

static u32 gl_copy_string(const char* text) {
  if (!text) return 0;
  u32 n = (u32)strlen(text) + 1;
  u32 p = gl_guest_alloc(n);
  if (p) memcpy(guest_ptr(p, n), text, n);
  return p;
}

/* GLsync is an opaque host pointer; the guest only carries a u32 handle. */
#define SYNC_MAX 64
static void* g_syncs[SYNC_MAX];
static u32 g_sync_ids[SYNC_MAX], g_sync_n, g_sync_next = 1;

static u32 sync_add(void* p) {
  if (!p || g_sync_n == SYNC_MAX) return 0;
  u32 id = g_sync_next++;
  g_sync_ids[g_sync_n] = id;
  g_syncs[g_sync_n++] = p;
  return id;
}

static void* sync_get(u32 id) {
  for (u32 i = 0; i < g_sync_n; ++i)
    if (g_sync_ids[i] == id) return g_syncs[i];
  return (void*)(uintptr_t)id;
}

static void sync_remove(u32 id) {
  for (u32 i = 0; i < g_sync_n; ++i)
    if (g_sync_ids[i] == id) {
      g_sync_ids[i] = g_sync_ids[--g_sync_n];
      g_syncs[i] = g_syncs[g_sync_n];
      return;
    }
}

bool gl_init(weblox_gl_proc_fn loader) {
  if (g_ready) return true;
  if (!loader) {
    fprintf(stderr, "[gl] no GL proc loader\n");
    return false;
  }
  /* Every entry point starts from SDL's proc loader (also covers extensions). */
#define GL_LOAD(name) p_gl_##name = loader("gl" #name);
  GL_FN_LIST(GL_LOAD)
#undef GL_LOAD
#if defined(WEBLOX_HAVE_GLAD_GLES2)
  /* Then prefer GLAD's resolved globals for the core GLES2/GLES3 set. */
  if (!gladLoadGLES2Loader((GLADloadproc)loader))
    fprintf(stderr, "[gl] GLAD GLES2 loader failed; using raw procs\n");
#define GL_USE_GLAD(name) p_gl_##name = (void*)glad_gl##name;
  GL_GLAD_LIST(GL_USE_GLAD)
#undef GL_USE_GLAD
#endif
  g_ready = 1;
  fprintf(stderr, "[gl] GLES dispatch ready\n");
  return true;
}

bool gl_ready(void) { return g_ready != 0; }

void gl_flush(void) {
  if (p_gl_Flush) ((void (*)(void))p_gl_Flush)();
}

void gl_shutdown(void) {
  if (g_map_host && g_map_guest) gl_guest_free(g_map_guest);
  g_map_host = NULL; g_map_guest = 0; g_map_len = 0;
  for (u32 i = 0; i < g_str_n; ++i) gl_guest_free(g_str_vals[i]);
  g_str_n = 0; g_sync_n = 0; g_ready = 0;
#define GL_CLEAR(name) p_gl_##name = NULL;
  GL_FN_LIST(GL_CLEAR)
#undef GL_CLEAR
}


u32 w2c_env_emscripten_webgl_create_context(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env; (void)a1; (void)a2;
  if (!gl_ready() && !gl_init(weblox_window_gl_loader())) {
    fprintf(stderr, "[gl] webgl_create_context: no GLES context\n");
    return 0;
  }
  return 1;
}

u32 w2c_env_emscripten_webgl_destroy_context(struct w2c_env* env, u32 a1) {
  (void)env; (void)a1;
  return 0;
}

u32 w2c_env_emscripten_webgl_get_context_attributes(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env; (void)a1;
  unsigned char* b = (unsigned char*)guest_ptr(a2, 40);
  if (!b) return (u32)-5;
  memset(b, 0, 40);
  b[0] = 1;  /* alpha */
  b[1] = 1;  /* depth */
  b[2] = 1;  /* stencil */
  b[3] = 1;  /* antialias */
  b[4] = 1;  /* premultipliedAlpha */
  b[5] = 0;  /* preserveDrawingBuffer */
  memcpy(b + 8, &(int){0}, 4);   /* powerPreference: default */
  b[12] = 0;                     /* failIfMajorPerformanceCaveat */
  memcpy(b + 16, &(int){2}, 4); /* majorVersion: WebGL2 / GLES3 */
  memcpy(b + 20, &(int){0}, 4); /* minorVersion */
  b[24] = 1;                     /* enableExtensionsByDefault */
  b[33] = 0;                     /* desynchronized */
  return 0;
}

u32 w2c_env_emscripten_webgl_make_context_current(struct w2c_env* env, u32 a1) {
  (void)env; (void)a1;
  /* Emscripten uses 0 for success and negative errno values for failure. */
  return gl_ready() ? 0 : (u32)-5;
}

void w2c_env_glActiveTexture(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(ActiveTexture)) { gl_miss("glActiveTexture"); return; }
  GL_V(ActiveTexture, u32)(a1);
}


void w2c_env_glAttachShader(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(AttachShader)) { gl_miss("glAttachShader"); return; }
  GL_V(AttachShader, u32, u32)(a1, a2);
}


void w2c_env_glBindAttribLocation(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!p_gl_BindAttribLocation) { gl_miss("glBindAttribLocation"); return; }
  GL_V(BindAttribLocation, u32, u32, const char*)(a1, a2, guest_str(a3));
}

void w2c_env_glBindBuffer(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(BindBuffer)) { gl_miss("glBindBuffer"); return; }
  GL_V(BindBuffer, u32, u32)(a1, a2);
}


void w2c_env_glBindBufferBase(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!GL_P(BindBufferBase)) { gl_miss("glBindBufferBase"); return; }
  GL_V(BindBufferBase, u32, u32, u32)(a1, a2, a3);
}


void w2c_env_glBindBufferRange(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) {
  (void)env;
  if (!GL_P(BindBufferRange)) { gl_miss("glBindBufferRange"); return; }
  GL_V(BindBufferRange, u32, u32, u32, u32, u32)(a1, a2, a3, a4, a5);
}


void w2c_env_glBindFramebuffer(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(BindFramebuffer)) { gl_miss("glBindFramebuffer"); return; }
  GL_V(BindFramebuffer, u32, u32)(a1, a2);
}


void w2c_env_glBindRenderbuffer(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(BindRenderbuffer)) { gl_miss("glBindRenderbuffer"); return; }
  GL_V(BindRenderbuffer, u32, u32)(a1, a2);
}


void w2c_env_glBindTexture(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(BindTexture)) { gl_miss("glBindTexture"); return; }
  GL_V(BindTexture, u32, u32)(a1, a2);
}


void w2c_env_glBindVertexArray(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(BindVertexArray)) { gl_miss("glBindVertexArray"); return; }
  GL_V(BindVertexArray, u32)(a1);
}


void w2c_env_glBlendFunc(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(BlendFunc)) { gl_miss("glBlendFunc"); return; }
  GL_V(BlendFunc, u32, u32)(a1, a2);
}


void w2c_env_glBlendFuncSeparate(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!GL_P(BlendFuncSeparate)) { gl_miss("glBlendFuncSeparate"); return; }
  GL_V(BlendFuncSeparate, u32, u32, u32, u32)(a1, a2, a3, a4);
}


void w2c_env_glBlitFramebuffer(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10) {
  (void)env;
  if (!GL_P(BlitFramebuffer)) { gl_miss("glBlitFramebuffer"); return; }
  GL_V(BlitFramebuffer, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}


void w2c_env_glBufferData(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  const void* data = guest_ptr(a3, a4);
  if (!data) { gl_badptr(); return; }
  if (!p_gl_BufferData) { gl_miss("glBufferData"); return; }
  GL_V(BufferData, u32, u32, const void*, u32)(a1, a2, data, a4);
}

void w2c_env_glBufferSubData(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  const void* data = guest_ptr(a3, a4);
  if (!data) { gl_badptr(); return; }
  if (!p_gl_BufferSubData) { gl_miss("glBufferSubData"); return; }
  GL_V(BufferSubData, u32, u32, const void*, u32)(a1, a2, data, a4);
}

u32 w2c_env_glCheckFramebufferStatus(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!p_gl_CheckFramebufferStatus) { gl_miss("glCheckFramebufferStatus"); return 0x8CD5u; }
  return GL_R(CheckFramebufferStatus, u32, u32)(a1);
}

void w2c_env_glClear(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(Clear)) { gl_miss("glClear"); return; }
  GL_V(Clear, u32)(a1);
}


void w2c_env_glClearBufferfi(struct w2c_env* env, u32 a1, u32 a2, f32 a3, u32 a4) {
  (void)env;
  if (!GL_P(ClearBufferfi)) { gl_miss("glClearBufferfi"); return; }
  GL_V(ClearBufferfi, u32, u32, f32, u32)(a1, a2, a3, a4);
}


void w2c_env_glClearBufferfv(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env; (void)a3;
  const float* values = (float const*)guest_ptr(a2, 4 * sizeof(float));
  if (!values) { gl_badptr(); return; }
  if (!p_gl_ClearBufferfv) { gl_miss("glClearBufferfv"); return; }
  GL_V(ClearBufferfv, u32, u32, const float*)(a1, a2, values);
}

void w2c_env_glClearBufferiv(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env; (void)a3;
  const int* values = (int const*)guest_ptr(a2, 4 * sizeof(int));
  if (!values) { gl_badptr(); return; }
  if (!p_gl_ClearBufferiv) { gl_miss("glClearBufferiv"); return; }
  GL_V(ClearBufferiv, u32, u32, const int*)(a1, a2, values);
}

void w2c_env_glClearColor(struct w2c_env* env, f32 a1, f32 a2, f32 a3, f32 a4) {
  (void)env;
  if (!GL_P(ClearColor)) { gl_miss("glClearColor"); return; }
  GL_V(ClearColor, f32, f32, f32, f32)(a1, a2, a3, a4);
}


void w2c_env_glClearDepthf(struct w2c_env* env, f32 a1) {
  (void)env;
  if (!GL_P(ClearDepthf)) { gl_miss("glClearDepthf"); return; }
  GL_V(ClearDepthf, f32)(a1);
}


void w2c_env_glClearStencil(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(ClearStencil)) { gl_miss("glClearStencil"); return; }
  GL_V(ClearStencil, u32)(a1);
}


u32 w2c_env_glClientWaitSync(struct w2c_env* env, u32 a1, u32 a2, u64 a3) {
  (void)env;
  if (!p_gl_ClientWaitSync) { gl_miss("glClientWaitSync"); return 0x911Au; }
  return GL_R(ClientWaitSync, u32, void*, u32, u64)(sync_get(a1), a2, a3);
}

void w2c_env_glClipControlEXT(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(ClipControlEXT)) { gl_miss("glClipControlEXT"); return; }
  GL_V(ClipControlEXT, u32, u32)(a1, a2);
}


void w2c_env_glColorMask(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!GL_P(ColorMask)) { gl_miss("glColorMask"); return; }
  GL_V(ColorMask, u32, u32, u32, u32)(a1, a2, a3, a4);
}


void w2c_env_glCompileShader(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(CompileShader)) { gl_miss("glCompileShader"); return; }
  GL_V(CompileShader, u32)(a1);
}


void w2c_env_glCompressedTexImage2D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8) {
  (void)env;
  const void* data = guest_ptr(a8, a7);
  if (!data) { gl_badptr(); return; }
  if (!p_gl_CompressedTexImage2D) { gl_miss("glCompressedTexImage2D"); return; }
  GL_V(CompressedTexImage2D, u32, u32, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, a6, a7, data);
}

void w2c_env_glCompressedTexImage3D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) {
  (void)env;
  const void* data = guest_ptr(a9, a8);
  if (!data) { gl_badptr(); return; }
  if (!p_gl_CompressedTexImage3D) { gl_miss("glCompressedTexImage3D"); return; }
  GL_V(CompressedTexImage3D, u32, u32, u32, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, a6, a7, a8, data);
}

void w2c_env_glCompressedTexSubImage2D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) {
  (void)env;
  const void* data = guest_ptr(a9, a8);
  if (!data) { gl_badptr(); return; }
  if (!p_gl_CompressedTexSubImage2D) { gl_miss("glCompressedTexSubImage2D"); return; }
  GL_V(CompressedTexSubImage2D, u32, u32, u32, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, a6, a7, a8, data);
}

void w2c_env_glCompressedTexSubImage3D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10, u32 a11) {
  (void)env;
  const void* data = guest_ptr(a11, a10);
  if (!data) { gl_badptr(); return; }
  if (!p_gl_CompressedTexSubImage3D) { gl_miss("glCompressedTexSubImage3D"); return; }
  GL_V(CompressedTexSubImage3D, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, data);
}

void w2c_env_glCopyTexSubImage2D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8) {
  (void)env;
  if (!GL_P(CopyTexSubImage2D)) { gl_miss("glCopyTexSubImage2D"); return; }
  GL_V(CopyTexSubImage2D, u32, u32, u32, u32, u32, u32, u32, u32)(a1, a2, a3, a4, a5, a6, a7, a8);
}


u32 w2c_env_glCreateProgram(struct w2c_env* env) {
  (void)env;
  if (!p_gl_CreateProgram) { gl_miss("glCreateProgram"); static u32 next = 1000; return next++; }
  return GL_R(CreateProgram, u32)( );
}

u32 w2c_env_glCreateShader(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!p_gl_CreateShader) { gl_miss("glCreateShader"); static u32 next = 1; return next++; }
  return GL_R(CreateShader, u32, u32)(a1);
}

void w2c_env_glCullFace(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(CullFace)) { gl_miss("glCullFace"); return; }
  GL_V(CullFace, u32)(a1);
}


void w2c_env_glDeleteBuffers(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_DeleteBuffers) { gl_miss("glDeleteBuffers"); return; }
  u32* ids = (u32*)guest_ptr(a2, (size_t)a1 * 4u);
  if (!ids) { gl_badptr(); return; }
  GL_V(DeleteBuffers, u32, const u32*)(a1, ids);
}

void w2c_env_glDeleteFramebuffers(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_DeleteFramebuffers) { gl_miss("glDeleteFramebuffers"); return; }
  u32* ids = (u32*)guest_ptr(a2, (size_t)a1 * 4u);
  if (!ids) { gl_badptr(); return; }
  GL_V(DeleteFramebuffers, u32, const u32*)(a1, ids);
}

void w2c_env_glDeleteProgram(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(DeleteProgram)) { gl_miss("glDeleteProgram"); return; }
  GL_V(DeleteProgram, u32)(a1);
}


void w2c_env_glDeleteRenderbuffers(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_DeleteRenderbuffers) { gl_miss("glDeleteRenderbuffers"); return; }
  u32* ids = (u32*)guest_ptr(a2, (size_t)a1 * 4u);
  if (!ids) { gl_badptr(); return; }
  GL_V(DeleteRenderbuffers, u32, const u32*)(a1, ids);
}

void w2c_env_glDeleteShader(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(DeleteShader)) { gl_miss("glDeleteShader"); return; }
  GL_V(DeleteShader, u32)(a1);
}


void w2c_env_glDeleteSync(struct w2c_env* env, u32 a1) {
  (void)env;
  void* sync = sync_get(a1);
  if (p_gl_DeleteSync) GL_V(DeleteSync, void*)(sync);
  else gl_miss("glDeleteSync");
  sync_remove(a1);
}

void w2c_env_glDeleteTextures(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_DeleteTextures) { gl_miss("glDeleteTextures"); return; }
  u32* ids = (u32*)guest_ptr(a2, (size_t)a1 * 4u);
  if (!ids) { gl_badptr(); return; }
  GL_V(DeleteTextures, u32, const u32*)(a1, ids);
}

void w2c_env_glDeleteVertexArrays(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_DeleteVertexArrays) { gl_miss("glDeleteVertexArrays"); return; }
  u32* ids = (u32*)guest_ptr(a2, (size_t)a1 * 4u);
  if (!ids) { gl_badptr(); return; }
  GL_V(DeleteVertexArrays, u32, const u32*)(a1, ids);
}

void w2c_env_glDepthFunc(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(DepthFunc)) { gl_miss("glDepthFunc"); return; }
  GL_V(DepthFunc, u32)(a1);
}


void w2c_env_glDepthMask(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(DepthMask)) { gl_miss("glDepthMask"); return; }
  GL_V(DepthMask, u32)(a1);
}


void w2c_env_glDisable(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(Disable)) { gl_miss("glDisable"); return; }
  GL_V(Disable, u32)(a1);
}


void w2c_env_glDisableVertexAttribArray(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(DisableVertexAttribArray)) { gl_miss("glDisableVertexAttribArray"); return; }
  GL_V(DisableVertexAttribArray, u32)(a1);
}


void w2c_env_glDrawArrays(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!GL_P(DrawArrays)) { gl_miss("glDrawArrays"); return; }
  GL_V(DrawArrays, u32, u32, u32)(a1, a2, a3);
}


void w2c_env_glDrawArraysInstanced(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!GL_P(DrawArraysInstanced)) { gl_miss("glDrawArraysInstanced"); return; }
  GL_V(DrawArraysInstanced, u32, u32, u32, u32)(a1, a2, a3, a4);
}


void w2c_env_glDrawBuffers(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_DrawBuffers) { gl_miss("glDrawBuffers"); return; }
  const int* bufs = (const int*)guest_ptr(a2, (size_t)a1 * 4u);
  if (!bufs) { gl_badptr(); return; }
  GL_V(DrawBuffers, u32, const int*)(a1, bufs);
}

void w2c_env_glDrawElements(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!p_gl_DrawElements) { gl_miss("glDrawElements"); return; }
  GL_V(DrawElements, u32, u32, u32, const void*)(a1, a2, a3, (const void*)(uintptr_t)a4);
}

void w2c_env_glDrawElementsInstanced(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) {
  (void)env;
  if (!p_gl_DrawElementsInstanced) { gl_miss("glDrawElementsInstanced"); return; }
  GL_V(DrawElementsInstanced, u32, u32, u32, const void*, u32)(a1, a2, a3, (const void*)(uintptr_t)a5, a4);
}

void w2c_env_glEnable(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(Enable)) { gl_miss("glEnable"); return; }
  GL_V(Enable, u32)(a1);
}


void w2c_env_glEnableVertexAttribArray(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(EnableVertexAttribArray)) { gl_miss("glEnableVertexAttribArray"); return; }
  GL_V(EnableVertexAttribArray, u32)(a1);
}


u32 w2c_env_glFenceSync(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_FenceSync) { gl_miss("glFenceSync"); return 0; }
  u32 id = sync_add(GL_R(FenceSync, void*, u32, u32)(a1, a2));
  if (!id) gl_miss("glFenceSync: sync table full");
  return id;
}

void w2c_env_glFramebufferRenderbuffer(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!GL_P(FramebufferRenderbuffer)) { gl_miss("glFramebufferRenderbuffer"); return; }
  GL_V(FramebufferRenderbuffer, u32, u32, u32, u32)(a1, a2, a3, a4);
}


void w2c_env_glFramebufferTexture2D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) {
  (void)env;
  if (!GL_P(FramebufferTexture2D)) { gl_miss("glFramebufferTexture2D"); return; }
  GL_V(FramebufferTexture2D, u32, u32, u32, u32, u32)(a1, a2, a3, a4, a5);
}


void w2c_env_glFramebufferTextureLayer(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) {
  (void)env;
  if (!GL_P(FramebufferTextureLayer)) { gl_miss("glFramebufferTextureLayer"); return; }
  GL_V(FramebufferTextureLayer, u32, u32, u32, u32, u32)(a1, a2, a3, a4, a5);
}


void w2c_env_glGenBuffers(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(GenBuffers)) { gl_miss("glGenBuffers"); return; }
  GL_V(GenBuffers, u32, u32)(a1, a2);
}


void w2c_env_glGenFramebuffers(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(GenFramebuffers)) { gl_miss("glGenFramebuffers"); return; }
  GL_V(GenFramebuffers, u32, u32)(a1, a2);
}


void w2c_env_glGenRenderbuffers(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(GenRenderbuffers)) { gl_miss("glGenRenderbuffers"); return; }
  GL_V(GenRenderbuffers, u32, u32)(a1, a2);
}


void w2c_env_glGenTextures(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(GenTextures)) { gl_miss("glGenTextures"); return; }
  GL_V(GenTextures, u32, u32)(a1, a2);
}


void w2c_env_glGenVertexArrays(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(GenVertexArrays)) { gl_miss("glGenVertexArrays"); return; }
  GL_V(GenVertexArrays, u32, u32)(a1, a2);
}


void w2c_env_glGenerateMipmap(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(GenerateMipmap)) { gl_miss("glGenerateMipmap"); return; }
  GL_V(GenerateMipmap, u32)(a1);
}


void w2c_env_glGetActiveUniform(struct w2c_env* env, u32 a1, u32 a2, u32 a3,
                               u32 a4, u32 a5, u32 a6, u32 a7) {
  (void)env;
  u32* length = (u32*)guest_ptr(a4, 4);
  u32* size = (u32*)guest_ptr(a5, 4);
  u32* type = (u32*)guest_ptr(a6, 4);
  char* name = (char*)guest_ptr(a7, a3);
  if (!p_gl_GetActiveUniform) {
    gl_miss("glGetActiveUniform");
    if (length) *length = 0;
    if (size) *size = 0;
    if (type) *type = 0;
    if (name) *name = 0;
    return;
  }
  GL_V(GetActiveUniform, u32, u32, u32, u32*, u32*, u32*, char*)(a1, a2, a3, length, size, type, name);
}

void w2c_env_glGetActiveUniformBlockiv(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!p_gl_GetActiveUniformBlockiv) { gl_miss("glGetActiveUniformBlockiv"); return; }
  int* params = (int*)guest_ptr(a4, 4);
  if (!params) { gl_badptr(); return; }
  GL_V(GetActiveUniformBlockiv, u32, u32, u32, int*)(a1, a2, a3, params);
}

u32 w2c_env_glGetError(struct w2c_env* env) {
  (void)env;
  if (!p_gl_GetError) { gl_miss("glGetError"); return 0; }
  return GL_R(GetError, u32)();
}

void w2c_env_glGetIntegerv(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  int* out = (int*)guest_ptr(a2, 4);
  if (!p_gl_GetIntegerv) {
    gl_miss("glGetIntegerv");
    if (out) *out = 0;
    return;
  }
  if (!out) { gl_badptr(); return; }
  GL_V(GetIntegerv, u32, int*)(a1, out);
}

void w2c_env_glGetProgramInfoLog(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  u32* len = (u32*)guest_ptr(a3, 4);
  char* log = (char*)guest_ptr(a4, a2);
  if (!p_gl_GetProgramInfoLog) {
    gl_miss("glGetProgramInfoLog");
    if (len) *len = 0;
    if (log && a2) *log = 0;
    return;
  }
  GL_V(GetProgramInfoLog, u32, u32, u32*, char*)(a1, a2, len, log);
}

void w2c_env_glGetProgramiv(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  int* out = (int*)guest_ptr(a3, 4);
  if (!p_gl_GetProgramiv) {
    gl_miss("glGetProgramiv");
    if (out) *out = 1;
    return;
  }
  if (!out) { gl_badptr(); return; }
  GL_V(GetProgramiv, u32, u32, int*)(a1, a2, out);
}

void w2c_env_glGetShaderInfoLog(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  u32* len = (u32*)guest_ptr(a3, 4);
  char* log = (char*)guest_ptr(a4, a2);
  if (!p_gl_GetShaderInfoLog) {
    gl_miss("glGetShaderInfoLog");
    if (len) *len = 0;
    if (log && a2) *log = 0;
    return;
  }
  GL_V(GetShaderInfoLog, u32, u32, u32*, char*)(a1, a2, len, log);
}

void w2c_env_glGetShaderiv(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  int* out = (int*)guest_ptr(a3, 4);
  if (!p_gl_GetShaderiv) {
    gl_miss("glGetShaderiv");
    if (out) *out = 1;
    return;
  }
  if (!out) { gl_badptr(); return; }
  GL_V(GetShaderiv, u32, u32, int*)(a1, a2, out);
}

u32 w2c_env_glGetString(struct w2c_env* env, u32 a1) {
  (void)env;
  for (u32 i = 0; i < g_str_n; ++i)
    if (g_str_keys[i] == a1) return g_str_vals[i];
  const char* fallback = "";
  switch (a1) {
    case 0x1F00u: fallback = "Weblox"; break;               /* GL_VENDOR */
    case 0x1F01u: fallback = "SDL3 GLES"; break;            /* GL_RENDERER */
    case 0x1F02u: fallback = "OpenGL ES 3.0"; break;        /* GL_VERSION */
    case 0x8B8Cu: fallback = "GLSL ES 3.00"; break;         /* GL_SHADING_LANGUAGE_VERSION */
    default: break;
  }
  const char* text = fallback;
  if (p_gl_GetString) {
    const char* driver = (const char*)GL_R(GetString, void*, u32)(a1);
    if (driver && *driver) text = driver;
  }
  u32 guest = gl_copy_string(text);
  if (g_str_n < 16) {
    g_str_keys[g_str_n] = a1;
    g_str_vals[g_str_n] = guest;
    ++g_str_n;
  }
  return guest;
}

u32 w2c_env_glGetUniformBlockIndex(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_GetUniformBlockIndex) { gl_miss("glGetUniformBlockIndex"); return 0xFFFFFFFFu; }
  return GL_R(GetUniformBlockIndex, u32, u32, const char*)(a1, guest_str(a2));
}

u32 w2c_env_glGetUniformLocation(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!p_gl_GetUniformLocation) { gl_miss("glGetUniformLocation"); return 0xFFFFFFFFu; }
  return GL_R(GetUniformLocation, int, u32, const char*)(a1, guest_str(a2));
}

void w2c_env_glInvalidateFramebuffer(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!p_gl_InvalidateFramebuffer) { gl_miss("glInvalidateFramebuffer"); return; }
  const int* lists = (const int*)guest_ptr(a3, (size_t)a2 * 4u);
  if (!lists) { gl_badptr(); return; }
  GL_V(InvalidateFramebuffer, u32, u32, const int*)(a1, a2, lists);
}

void w2c_env_glLinkProgram(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(LinkProgram)) { gl_miss("glLinkProgram"); return; }
  GL_V(LinkProgram, u32)(a1);
}


u32 w2c_env_glMapBufferRange(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!p_gl_MapBufferRange) { gl_miss("glMapBufferRange"); return 0; }
  void* host = GL_R(MapBufferRange, void*, u32, u32, u32, u32)(a1, a2, a3, a4);
  if (!host) return 0;
  u32 guest = gl_guest_alloc(a3 ? a3 : 1);
  if (!guest) return 0;
  if (a3) memcpy(guest_ptr(guest, a3), host, a3);
  g_map_host = host; g_map_guest = guest; g_map_len = a3;
  return guest;
}

void w2c_env_glPixelStorei(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(PixelStorei)) { gl_miss("glPixelStorei"); return; }
  GL_V(PixelStorei, u32, u32)(a1, a2);
}


void w2c_env_glPolygonOffset(struct w2c_env* env, f32 a1, f32 a2) {
  (void)env;
  if (!GL_P(PolygonOffset)) { gl_miss("glPolygonOffset"); return; }
  GL_V(PolygonOffset, f32, f32)(a1, a2);
}


void w2c_env_glReadPixels(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7) {
  (void)env;
  if (!p_gl_ReadPixels) { gl_miss("glReadPixels"); return; }
  size_t need = (size_t)a3 * a4 * 4u;
  void* pixels = guest_ptr(a7, need ? need : 1);
  if (!pixels) { gl_badptr(); return; }
  GL_V(ReadPixels, u32, u32, u32, u32, u32, u32, void*)(a1, a2, a3, a4, a5, a6, pixels);
}

void w2c_env_glRenderbufferStorage(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!GL_P(RenderbufferStorage)) { gl_miss("glRenderbufferStorage"); return; }
  GL_V(RenderbufferStorage, u32, u32, u32, u32)(a1, a2, a3, a4);
}


void w2c_env_glRenderbufferStorageMultisample(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) {
  (void)env;
  if (!GL_P(RenderbufferStorageMultisample)) { gl_miss("glRenderbufferStorageMultisample"); return; }
  GL_V(RenderbufferStorageMultisample, u32, u32, u32, u32, u32)(a1, a2, a3, a4, a5);
}


void w2c_env_glScissor(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!GL_P(Scissor)) { gl_miss("glScissor"); return; }
  GL_V(Scissor, u32, u32, u32, u32)(a1, a2, a3, a4);
}


void w2c_env_glShaderSource(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!p_gl_ShaderSource) { gl_miss("glShaderSource"); return; }
  const char* one[1];
  const char** arr = a2 <= 1 ? one : (const char**)calloc(a2, sizeof(char*));
  if (!arr) return;
  for (u32 i = 0; i < a2; ++i) {
    const unsigned char* slot = guest_ptr(a3 + i * 4u, 4);
    u32 str = slot ? (u32)slot[0] | ((u32)slot[1] << 8) | ((u32)slot[2] << 16) | ((u32)slot[3] << 24) : 0;
    arr[i] = guest_str(str);
  }
  const int* lengths = a4 ? (const int*)guest_ptr(a4, (size_t)a2 * 4u) : NULL;
  GL_V(ShaderSource, u32, u32, const char**, const int*)(a1, a2, arr, lengths);
  if (arr != one) free((void*)arr);
}

void w2c_env_glStencilFunc(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!GL_P(StencilFunc)) { gl_miss("glStencilFunc"); return; }
  GL_V(StencilFunc, u32, u32, u32)(a1, a2, a3);
}


void w2c_env_glStencilMask(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(StencilMask)) { gl_miss("glStencilMask"); return; }
  GL_V(StencilMask, u32)(a1);
}


void w2c_env_glStencilOp(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!GL_P(StencilOp)) { gl_miss("glStencilOp"); return; }
  GL_V(StencilOp, u32, u32, u32)(a1, a2, a3);
}


void w2c_env_glTexImage2D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) {
  (void)env;
  if (!p_gl_TexImage2D) { gl_miss("glTexImage2D"); return; }
  const void* pixels = guest_ptr(a9, (size_t)a4 * a5 * 4u);
  if (!pixels) { gl_badptr(); return; }
  GL_V(TexImage2D, u32, u32, u32, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, a6, a7, a8, pixels);
}

void w2c_env_glTexImage3D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10) {
  (void)env;
  if (!p_gl_TexImage3D) { gl_miss("glTexImage3D"); return; }
  const void* pixels = guest_ptr(a10, (size_t)a4 * a5 * a6 * 4u);
  if (!pixels) { gl_badptr(); return; }
  GL_V(TexImage3D, u32, u32, u32, u32, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, a6, a7, a8, a9, pixels);
}

void w2c_env_glTexParameterf(struct w2c_env* env, u32 a1, u32 a2, f32 a3) {
  (void)env;
  if (!GL_P(TexParameterf)) { gl_miss("glTexParameterf"); return; }
  GL_V(TexParameterf, u32, u32, f32)(a1, a2, a3);
}


void w2c_env_glTexParameterfv(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!p_gl_TexParameterfv) { gl_miss("glTexParameterfv"); return; }
  const float* values = (const float*)guest_ptr(a3, 4 * sizeof(float));
  if (!values) { gl_badptr(); return; }
  GL_V(TexParameterfv, u32, u32, const float*)(a1, a2, values);
}

void w2c_env_glTexParameteri(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!GL_P(TexParameteri)) { gl_miss("glTexParameteri"); return; }
  GL_V(TexParameteri, u32, u32, u32)(a1, a2, a3);
}


void w2c_env_glTexStorage2D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) {
  (void)env;
  if (!GL_P(TexStorage2D)) { gl_miss("glTexStorage2D"); return; }
  GL_V(TexStorage2D, u32, u32, u32, u32, u32)(a1, a2, a3, a4, a5);
}


void w2c_env_glTexStorage3D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6) {
  (void)env;
  if (!GL_P(TexStorage3D)) { gl_miss("glTexStorage3D"); return; }
  GL_V(TexStorage3D, u32, u32, u32, u32, u32, u32)(a1, a2, a3, a4, a5, a6);
}


void w2c_env_glTexSubImage2D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9) {
  (void)env;
  if (!p_gl_TexSubImage2D) { gl_miss("glTexSubImage2D"); return; }
  const void* pixels = guest_ptr(a9, (size_t)a4 * a5 * 4u);
  if (!pixels) { gl_badptr(); return; }
  GL_V(TexSubImage2D, u32, u32, u32, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, a6, a7, a8, pixels);
}

void w2c_env_glTexSubImage3D(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9, u32 a10, u32 a11) {
  (void)env;
  if (!p_gl_TexSubImage3D) { gl_miss("glTexSubImage3D"); return; }
  const void* pixels = guest_ptr(a11, (size_t)a4 * a5 * a6 * 4u);
  if (!pixels) { gl_badptr(); return; }
  GL_V(TexSubImage3D, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, pixels);
}

void w2c_env_glUniform1i(struct w2c_env* env, u32 a1, u32 a2) {
  (void)env;
  if (!GL_P(Uniform1i)) { gl_miss("glUniform1i"); return; }
  GL_V(Uniform1i, u32, u32)(a1, a2);
}


void w2c_env_glUniformBlockBinding(struct w2c_env* env, u32 a1, u32 a2, u32 a3) {
  (void)env;
  if (!GL_P(UniformBlockBinding)) { gl_miss("glUniformBlockBinding"); return; }
  GL_V(UniformBlockBinding, u32, u32, u32)(a1, a2, a3);
}


u32 w2c_env_glUnmapBuffer(struct w2c_env* env, u32 a1) {
  (void)env;
  if (g_map_host) {
    if (g_map_len && g_map_guest) {
      void* dst = guest_ptr(g_map_guest, g_map_len);
      if (dst) memcpy(g_map_host, dst, g_map_len);
    }
    gl_guest_free(g_map_guest);
    g_map_host = NULL; g_map_guest = 0; g_map_len = 0;
  }
  if (!p_gl_UnmapBuffer) { gl_miss("glUnmapBuffer"); return 1; }
  return GL_R(UnmapBuffer, u32, u32)(a1);
}

void w2c_env_glUseProgram(struct w2c_env* env, u32 a1) {
  (void)env;
  if (!GL_P(UseProgram)) { gl_miss("glUseProgram"); return; }
  GL_V(UseProgram, u32)(a1);
}


void w2c_env_glVertexAttribPointer(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6) {
  (void)env;
  if (!p_gl_VertexAttribPointer) { gl_miss("glVertexAttribPointer"); return; }
  /* WebGL passes a byte offset into the bound VBO, not a linear-memory address. */
  GL_V(VertexAttribPointer, u32, u32, u32, u32, u32, const void*)(a1, a2, a3, a4, a5, (const void*)(uintptr_t)a6);
}

void w2c_env_glViewport(struct w2c_env* env, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)env;
  if (!GL_P(Viewport)) { gl_miss("glViewport"); return; }
  GL_V(Viewport, u32, u32, u32, u32)(a1, a2, a3, a4);
}

