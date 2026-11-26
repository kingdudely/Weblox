/* impl/glue.c - EM_ASM dispatch and mailbox.
 *
 * ASM_CONSTS are not strings in linear memory: the module passes a numeric key
 * and the JS glue indexes a table of closures with it (Lr in WebClient.js),
 * together with a *signature string* and a pointer to the packed arguments.
 * The glue decodes the signature (yt) and spreads the values over the closure.
 *
 * The closures are mostly DOM/audio/webview glue plus the engine's hooks into
 * the page (join status, join failure, crash report).  We implement everything
 * with an observable effect on the web client's control flow - especially the
 * join/exit hooks - and no-op the DOM ones.
 */
#include "../host/host.h"
#include <sys/random.h>

enum { A_I32 = 'i', A_F32 = 'f', A_F64 = 'd', A_I64 = 'j', A_PTR = 'p' };

typedef struct {
  int kind;
  union { u32 i; u64 j; double d; u32 p; } v;
} arg_t;

#define ARG_MAX 8

/* yt(): the signature is a C string; integer and pointer args advance 4,
 * 64-bit ones advance 8 and keep the cursor 8-aligned. */
static int decode_args(u32 sigp, u32 buf, arg_t* out) {
  const char* sig = sigp ? (const char*)wc_ptr(sigp) : "";
  if (!sig) return 0;
  int n = 0;
  u32 off = 0;
  for (const char* c = sig; *c && n < ARG_MAX; c++) {
    int wide = (*c == A_I64 || *c == A_F64);
    if (wide && (off & 7)) off += 4;
    arg_t a = { *c, { 0 } };
    switch (*c) {
      case A_I32: a.v.i = *(u32*)wc_ptr(buf + off); break;
      case A_F32: a.v.d = *(float*)wc_ptr(buf + off); break;
      case A_F64: a.v.d = *(double*)wc_ptr(buf + off); break;
      case A_I64: a.v.j = *(u64*)wc_ptr(buf + off); break;
      default:    a.v.p = *(u32*)wc_ptr(buf + off); break;
    }
    out[n++] = a;
    off += wide ? 8 : 4;
  }
  return n;
}

static double mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static unsigned seen_key[64];
static int n_seen;

static void unknown(u32 key) {
  for (int i = 0; i < n_seen; i++)
    if (seen_key[i] == key) return;
  if (n_seen < 64) seen_key[n_seen++] = key;
  fprintf(stderr, "[asmc] unknown key %u\n", key);
}

static const char* str_arg(u32 p) { return p ? wc_str(p) : ""; }

static void emit(u32 key, const char* name, const arg_t* a, int n) {
  fprintf(stderr, "[asm] %s(", name);
  for (int i = 0; i < n; i++) {
    switch (a[i].kind) {
      case A_PTR: fprintf(stderr, "\"%s\"", wc_str(a[i].v.p)); break;
      case A_I64: case A_F64: fprintf(stderr, "%.17g", a[i].v.d); break;
      default:    fprintf(stderr, "%d", (int)a[i].v.i); break;
    }
    if (i + 1 < n) fprintf(stderr, ", ");
  }
  fprintf(stderr, ")\n");
  (void)key;
}

static double eval(u32 key, u32 sigp, u32 argbuf) {
  arg_t a[ARG_MAX];
  int n = argbuf ? decode_args(sigp, argbuf, a) : 0;

  switch (key) {
    case 8667121: return mono_ms();                       /* performance.now */
    case 8667344: return 0.0;                             /* wasmPreInitTime */
    case 8667510: return 0.0;                             /* navigator.webdriver */
    case 8667550:                                         /* main() entered */
      fprintf(stderr, "[asm] main entered\n");
      return 0.0;
    case 8687293: return 0.0;                             /* HaveOffsetConverter */
    case 8687350: {                                       /* getRandomValue */
      u32 v;
      if (getrandom(&v, 4, 0) != 4) v = (u32)mono_ms();
      return (double)v;
    }
    case 8687386:                                         /* install getRandomValue */
      emit(key, "random_init", a, n);
      return 0.0;

    /* engine -> page hooks */
    case 8667715: {                                       /* loading status */
      const char* msg = str_arg(n > 2 ? a[2].v.p : 0);
      fprintf(stderr, "[asm] loading %s source=%d msg=\"%s\" err=%d status=%d\n",
              (n > 0 && a[0].v.i) ? "FAILURE" : "status",
              n > 1 ? (int)a[1].v.i : -1, msg,
              n > 3 ? (int)a[3].v.i : -1, n > 4 ? (int)a[4].v.i : -1);
      return 0.0;
    }
    case 8667386:                                         /* resolveGameExit join_failed */
      fprintf(stderr, "[asm] JOIN FAILED statusCode=%d\n", n ? (int)a[0].v.i : -1);
      return 0.0;
    case 8666895:                                         /* teleport_failed */
      fprintf(stderr, "[asm] TELEPORT FAILED statusCode=%d\n", n ? (int)a[0].v.i : -1);
      return 0.0;
    case 8679911:                                         /* resolveGameExit crash */
      fprintf(stderr, "[asm] CRASH type=%s msg=%s\n",
              str_arg(n > 0 ? a[0].v.p : 0), str_arg(n > 1 ? a[1].v.p : 0));
      return 0.0;
    case 8667151:                                         /* onExperienceJoined */
      fprintf(stderr, "[asm] EXPERIENCE JOINED\n");
      return 0.0;
    case 8667221:                                         /* onExperienceLeft */
      fprintf(stderr, "[asm] EXPERIENCE LEFT\n");
      return 0.0;
    case 8666972:                                         /* loading screen removed */
      fprintf(stderr, "[asm] loading screen removed\n");
      return 0.0;
    case 8667459: case 8667064: case 8667287:             /* leave / native_exit */
      fprintf(stderr, "[asm] exit (%u)\n", key);
      return 0.0;
    case 8686787:                                         /* cors header warning */
      fprintf(stderr, "[asm] cors: non-allowlisted header %s\n",
              str_arg(n > 0 ? a[0].v.p : 0));
      return 0.0;
    case 8686709:                                         /* saveIxpSettings */
      return 0.0;
    case 8686884:                                         /* microprofile dump */
      fprintf(stderr, "[asm] microprofile dump\n");
      return 0.0;

    /* FMOD: the mixer runs in an AudioWorklet we do not have, so the driver
     * only needs plausible sample rates and "no worklet". */
    case 8683155: case 8686439: return 48000.0;
    case 8680027: return 48000.0;
    case 8680489: return 0.0;
    case 8680778: return 0.0;
    case 8681161: return 0.0;
    case 8681223: case 8681495: case 8681910: case 8682303: case 8682589:
    case 8683356: case 8683776: case 8683994: case 8684412: case 8685416:
    case 8685627: case 8685650: case 8685888: case 8686089: case 8686142:
    case 8686417: case 8686645: case 8682631:
      return 0.0;

    /* webview / tab prompt */
    case 8668001: case 8670980: case 8671094: case 8671524: case 8673319:
    case 8674510: case 8678994: case 8679585:
      return 0.0;

    default:
      unknown(key);
      return 0.0;
  }
}

u32 wc_impl_emscripten_asm_const_int(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0;
  return (u32)eval(a1, a2, a3);
}

u32 wc_impl_emscripten_asm_const_int_sync_on_main_thread(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  extern __thread int g_is_main;
  if (!g_is_main) {
    /* the glue marshals to the main thread (M(0, ...)); the closures here are
     * side-effect only, so run them inline */
  }
  return wc_impl_emscripten_asm_const_int(a0, a1, a2, a3);
}

f64 wc_impl_emscripten_asm_const_double(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0;
  return eval(a1, a2, a3);
}

void wc_impl_emscripten_asm_const_async_on_main_thread(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0; (void)a1; (void)a2; (void)a3;
}

extern void wc_mailbox_await(u32 p);
void wc_impl_0x5Femscripten_thread_mailbox_await(struct w2c_env* a0, u32 a1) {
  (void)a0;
  wc_mailbox_await(a1);
}

/* A worker posted a mailbox item; the target replays it here (check_mailbox).
 * mo(id, fn, caller_thread, n, types, values, result) - `types` is a byte
 * offset into an array of 8-byte type tags, `values` holds the arguments the
 * same way.  Everything we know is synchronous, so run it inline and store the
 * result through `result`. */
f64 wc_impl_0x5Femscripten_receive_on_main_thread_js(struct w2c_env* a0, u32 a1, u32 a2, u32 a3,
                                                    u32 a4, u32 a5, u32 a6, u32 a7) {
  (void)a0; (void)a2; (void)a3;
  u32 n = a4;
  if (n > ARG_MAX) n = ARG_MAX;
  u32 pack[ARG_MAX];
  for (u32 i = 0; i < n; i++) {
    u64 tag = a5 ? *(u64*)wc_ptr(a5 + i * 8) : 0;
    u64 raw = a6 ? *(u64*)wc_ptr(a6 + i * 8) : 0;
    pack[i] = (tag == 2) ? (u32)(*(u64*)&raw) : (u32)raw;
  }
  /* our eval() decodes by signature; hand it an int/pointer signature */
  char sig[ARG_MAX + 1];
  for (u32 i = 0; i < n; i++) sig[i] = 'i';
  sig[n] = 0;
  u32 sp = n ? w2c_WebClient_malloc(g_inst, n * 4 + 1) : 0;
  u32 bp = n ? w2c_WebClient_malloc(g_inst, n * 4) : 0;
  f64 r = 0;
  if (sp && bp) {
    memcpy(wc_ptr(sp), sig, n + 1);
    memcpy(wc_ptr(bp), pack, n * 4);
    r = eval(a1, sp, bp);
  }
  if (sp) w2c_WebClient_free(g_inst, sp);
  if (bp) w2c_WebClient_free(g_inst, bp);
  if (a7) *(f64*)wc_ptr(a7) = r;
  return r;
}

/* __assert_fail: the glue logs and *throws* (`Pe(...)` builds an Error and the
 * thrown value unwinds the wasm stack), so a failed assert must not return. */
extern void wasm_rt_load_exception(const void* tag, uint32_t size, const void* values);
extern void wasm_rt_throw(void);
static int g_assert_tag;
void wc_impl_0x5F_assert_fail(struct w2c_env* a0, u32 cond, u32 file, u32 line,
                              u32 func) {
  (void)a0;
  static char buf[512];
  snprintf(buf, sizeof buf, "Assertion failed: %s, at: %s:%u %s",
           cond ? wc_str(cond) : "?", file ? wc_str(file) : "unknown filename",
           line, func ? wc_str(func) : "unknown function");
  fprintf(stderr, "[assert] %s\n", buf);
  wasm_rt_load_exception((const void*)&g_assert_tag, (uint32_t)strlen(buf) + 1, buf);
  wasm_rt_throw();
}
