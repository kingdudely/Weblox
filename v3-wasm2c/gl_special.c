/* impl/gl_special.c - GL calls that need sensible answers without a driver,
 * plus the emscripten_webgl_* context API.  Everything else is generated.
 */
#include "../host/host.h"
#include "../host/gl.h"

#define GL_VENDOR 0x1F00u
#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_SL_VERSION 0x8B8Cu

static void* proc(const char* n) { return wc_gl_proc(n); }

/* Driver strings live outside linear memory, so they have to be copied in and
 * the guest gets the copied address (the browser glue does the same with
 * UTF8ToString allocations). */
static u32 copy_str_in(const char* v) {
  if (!v) return 0;
  size_t n = strlen(v) + 1;
  u32 p = w2c_WebClient_malloc(g_inst, (u32)n);
  if (p) memcpy(wc_ptr(p), v, n);
  return p;
}

u32 wc_impl_glGetString(struct w2c_env* a0, u32 a1) {
  (void)a0;
  static u32 cache[16];
  static u32 keys[16];
  static int n;
  for (int i = 0; i < n; i++)
    if (keys[i] == a1) return cache[i];
  if (wc_gl_ready()) {
    void* p = proc("glGetString");
    if (p) {
      const char* v = (const char*)((void* (*)(u32))p)(a1);
      if (v) {
        u32 r = copy_str_in(v);
        if (n < 16) { keys[n] = a1; cache[n++] = r; }
        return r;
      }
    }
  }
  switch (a1) {
    case GL_VENDOR: return wc_gl_string(WC_GL_VENDOR);
    case GL_RENDERER: return wc_gl_string(WC_GL_RENDERER);
    case GL_VERSION: return wc_gl_string(WC_GL_VERSION);
    case GL_SL_VERSION: return wc_gl_string(WC_GL_SL_VERSION);
    default: return 0;
  }
}

u32 wc_impl_glGetStringi(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  void* p = proc("glGetStringi");
  if (!p) return 0;
  return copy_str_in((const char*)((void* (*)(u32, u32))p)(a1, a2));
}

void wc_impl_glGetIntegerv(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  void* p = proc("glGetIntegerv");
  { static int d; if (d++ < 20) fprintf(stderr, "[gli] p=%p a1=%x dst=%x ready=%d\n", p, a1, a2, wc_gl_ready()); }
  if (p) { ((void (*)(u32, u32))p)(a1, (u32)(uintptr_t)wc_ptr(a2)); return; }
  if (a2) *(u32*)wc_ptr(a2) = 0;
}

u32 wc_impl_glGetError(struct w2c_env* a0) {
  (void)a0;
  void* p = proc("glGetError");
  return p ? ((u32 (*)(void))p)() : 0;
}

u32 wc_impl_glCreateShader(struct w2c_env* a0, u32 a1) {
  (void)a0;
  void* p = proc("glCreateShader");
  if (p) return ((u32 (*)(u32))p)(a1);
  static u32 next = 1;
  return next++;
}

u32 wc_impl_glCreateProgram(struct w2c_env* a0) {
  (void)a0;
  void* p = proc("glCreateProgram");
  if (p) return ((u32 (*)(void))p)();
  static u32 next = 1000;
  return next++;
}

u32 wc_impl_glGetUniformLocation(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  void* p = proc("glGetUniformLocation");
  return p ? ((u32 (*)(u32, u32))p)(a1, (u32)(uintptr_t)wc_ptr(a2)) : 0;
}

u32 wc_impl_glGetUniformBlockIndex(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  void* p = proc("glGetUniformBlockIndex");
  return p ? ((u32 (*)(u32, u32))p)(a1, (u32)(uintptr_t)wc_ptr(a2)) : 0;
}

void wc_impl_glGetActiveUniform(struct w2c_env* a0, u32 a1, u32 a2, u32 a3,
                               u32 a4, u32 a5, u32 a6, u32 a7) {
  (void)a0;
  void* p = proc("glGetActiveUniform");
  if (!p) return;
  ((void (*)(u32, u32, u32, u32, u32, u32, u32))p)(a1, a2, a3,
      (u32)(uintptr_t)wc_ptr(a4), (u32)(uintptr_t)wc_ptr(a5),
      (u32)(uintptr_t)wc_ptr(a6), (u32)(uintptr_t)wc_ptr(a7));
}

void wc_impl_glGetShaderiv(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0;
  void* p = proc("glGetShaderiv");
  if (p) { ((void (*)(u32, u32, u32))p)(a1, a2, (u32)(uintptr_t)wc_ptr(a3)); return; }
  if (a3) *(u32*)wc_ptr(a3) = 1;   /* compile "succeeded" without a driver */
}

void wc_impl_glGetProgramiv(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0;
  void* p = proc("glGetProgramiv");
  if (p) { ((void (*)(u32, u32, u32))p)(a1, a2, (u32)(uintptr_t)wc_ptr(a3)); return; }
  if (a3) *(u32*)wc_ptr(a3) = 1;
}

void wc_impl_glGetShaderInfoLog(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)a0;
  void* p = proc("glGetShaderInfoLog");
  if (p) { ((void (*)(u32, u32, u32, u32))p)(a1, a2, (u32)(uintptr_t)wc_ptr(a3), (u32)(uintptr_t)wc_ptr(a4)); return; }
  if (a3) *(u32*)wc_ptr(a3) = 0;
}

void wc_impl_glGetProgramInfoLog(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)a0;
  void* p = proc("glGetProgramInfoLog");
  if (p) { ((void (*)(u32, u32, u32, u32))p)(a1, a2, (u32)(uintptr_t)wc_ptr(a3), (u32)(uintptr_t)wc_ptr(a4)); return; }
  if (a3) *(u32*)wc_ptr(a3) = 0;
}

u32 wc_impl_glCheckFramebufferStatus(struct w2c_env* a0, u32 a1) {
  (void)a0;
  void* p = proc("glCheckFramebufferStatus");
  return p ? ((u32 (*)(u32))p)(a1) : 0x8CD5u; /* FRAMEBUFFER_COMPLETE */
}

void wc_impl_glShaderSource(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)a0;
  void* p = proc("glShaderSource");
  if (!p) return;
  /* a2 is a count and a3 an array of *guest* string pointers */
  const char* one[1];
  const char** arr = one;
  if (a2 > 1) arr = (const char**)malloc(sizeof(char*) * a2);
  if (!arr) return;
  for (u32 i = 0; i < a2; i++)
    arr[i] = (const char*)wc_ptr(*(u32*)wc_ptr(a3 + i * 4));
  ((void (*)(u32, u32, const char**, u32))p)(a1, a2, arr,
      (u32)(uintptr_t)wc_ptr(a4));
  if (arr != one) free((void*)arr);
}

static void gen_names(const char* fn, u32 a1, u32 a2) {
  void* p = proc(fn);
  if (p) ((void (*)(u32, u32))p)(a1, (u32)(uintptr_t)wc_ptr(a2));
  else if (a2) *(u32*)wc_ptr(a2) = 1;
}

void wc_impl_glGenTextures(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; gen_names("glGenTextures", a1, a2); }
void wc_impl_glGenBuffers(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; gen_names("glGenBuffers", a1, a2); }
void wc_impl_glGenFramebuffers(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; gen_names("glGenFramebuffers", a1, a2); }
void wc_impl_glGenRenderbuffers(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; gen_names("glGenRenderbuffers", a1, a2); }
void wc_impl_glGenVertexArrays(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; gen_names("glGenVertexArrays", a1, a2); }

/* GLsync is a pointer but the guest only carries a u32, so keep a table. */
static void* g_syncs[64];
static u32 g_sync_ids[64];
static u32 g_sync_n;

u32 wc_impl_glFenceSync(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0;
  void* p = proc("glFenceSync");
  void* s = p ? ((void* (*)(u32, u32))p)(a1, a2) : NULL;
  static u32 n = 1;
  u32 id = n++;
  if (s && g_sync_n < 64) { g_syncs[g_sync_n] = s; g_sync_ids[g_sync_n++] = id; }
  return id;
}

static void* sync_for(u32 id) {
  for (u32 i = 0; i < g_sync_n; i++) if (g_sync_ids[i] == id) return g_syncs[i];
  return (void*)(uintptr_t)id;
}

u32 wc_impl_glClientWaitSync(struct w2c_env* a0, u32 a1, u32 a2, u64 a3) {
  (void)a0;
  void* p = proc("glClientWaitSync");
  return p ? ((u32 (*)(void*, u32, u64))p)(sync_for(a1), a2, a3) : 0x911Au;
}

void wc_impl_glDeleteSync(struct w2c_env* a0, u32 a1) {
  (void)a0;
  void* p = proc("glDeleteSync");
  if (p) ((void (*)(void*))p)(sync_for(a1));
  for (u32 i = 0; i < g_sync_n; i++)
    if (g_sync_ids[i] == a1) { g_sync_ids[i] = g_sync_ids[--g_sync_n]; g_syncs[i] = g_syncs[g_sync_n]; break; }
}

/* A mapping lives in host memory, but the guest reads it as a linear-memory
 * offset, so hand it a copy and write that back on unmap. */
static void* g_map_host;
static u32 g_map_guest, g_map_len;

u32 wc_impl_glMapBufferRange(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)a0;
  void* p = proc("glMapBufferRange");
  if (!p) return 0;
  void* host = ((void* (*)(u32, u32, u32, u32))p)(a1, a2, a3, a4);
  if (!host) return 0;
  u32 g = w2c_WebClient_malloc(g_inst, a3 ? a3 : 1);
  if (!g) return 0;
  if (a3) memcpy(wc_ptr(g), host, a3);
  g_map_host = host; g_map_guest = g; g_map_len = a3;
  return g;
}

u32 wc_impl_glUnmapBuffer(struct w2c_env* a0, u32 a1) {
  (void)a0;
  void* p = proc("glUnmapBuffer");
  if (g_map_host) {
    if (g_map_len && g_map_guest) memcpy(g_map_host, wc_ptr(g_map_guest), g_map_len);
    w2c_WebClient_free(g_inst, g_map_guest);
    g_map_host = NULL; g_map_guest = 0; g_map_len = 0;
  }
  return p ? ((u32 (*)(u32))p)(a1) : 1;
}

/* ---- emscripten_webgl_* ---- */

u32 wc_impl_emscripten_webgl_create_context(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0; (void)a1; (void)a2;
  if (!wc_gl_init(1280, 720)) return 0;
  return 1;
}

u32 wc_impl_emscripten_webgl_destroy_context(struct w2c_env* a0, u32 a1) {
  (void)a0; (void)a1;
  wc_gl_shutdown();
  return 0;
}

/* emscripten's webgl calls answer 0 on success and a negative errno on
 * failure (-5 = INVALID_VALUE); returning 1 made every mode look broken. */
u32 wc_impl_emscripten_webgl_make_context_current(struct w2c_env* a0, u32 a1) {
  (void)a0; (void)a1;
  return wc_gl_make_current() ? 0 : (u32)-5;
}

/* EmscriptenWebGLContextAttributes, as the glue fills it from
 * getContextAttributes(): u8 flags, i32 powerPreference, u8
 * failIfMajorPerformanceCaveat, i32 version, i32 minorVersion, u8
 * enableExtensionsByDefault, ..., u8 desynchronized. */
u32 wc_impl_emscripten_webgl_get_context_attributes(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0; (void)a1;
  if (!a2) return (u32)-5;
  unsigned char* b = wc_ptr(a2);
  memset(b, 0, 40);
  b[0] = 0;             /* alpha */
  b[1] = 1;             /* depth */
  b[2] = 1;             /* stencil */
  b[3] = 1;             /* antialias */
  b[4] = 1;             /* premultipliedAlpha */
  b[5] = 0;             /* preserveDrawingBuffer */
  *(int*)(b + 8) = 0;   /* powerPreference: default */
  b[12] = 0;            /* failIfMajorPerformanceCaveat */
  *(int*)(b + 16) = 2;  /* majorVersion: webgl2 */
  *(int*)(b + 20) = 0;  /* minorVersion */
  b[24] = 1;            /* enableExtensionsByDefault */
  b[33] = 0;            /* desynchronized */
  return 0;
}
