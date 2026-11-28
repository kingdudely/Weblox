// Emscripten EM_ASM dispatch, ported from v3-wasm2c/glue.c and the proven C++
// host (cpp/asm_const.cpp).  The keys are page-side closure IDs, not wasm
// indices; we implement the ones with an observable effect on client control
// flow (join/exit/crash hooks, timing, RNG, audio sample rate) and log the rest
// once each.  All guest reads go through guest.h - no hardcoded addresses.

/* clock_gettime() is POSIX, not ISO C; ask for it before any system header. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "asm_const.h"
#include "guest.h"
#include "abort_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <sys/types.h>
#endif

enum { A_I32 = 'i', A_F32 = 'f', A_F64 = 'd', A_I64 = 'j', A_PTR = 'p' };

typedef struct {
  char kind;
  union {
    u32 i;
    u64 j;
    double d;
    u32 p;
  } v;
} Arg;

#define ARG_MAX 8

/* ---------------------------------------------------------------- clock -- */

static int g_clock_ready;
#if defined(_WIN32)
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_start;
#else
static struct timespec g_start;
#endif

void weblox_asm_const_init(void) {
  if (g_clock_ready) return;
  g_clock_ready = 1;
#if defined(_WIN32)
  if (!QueryPerformanceFrequency(&g_freq)) g_freq.QuadPart = 0;
  QueryPerformanceCounter(&g_start);
#else
  clock_gettime(CLOCK_MONOTONIC, &g_start);
#endif
}

/* performance.now(): milliseconds since process start, monotonic. */
static double now_ms(void) {
  if (!g_clock_ready) weblox_asm_const_init();
#if defined(_WIN32)
  LARGE_INTEGER now;
  if (g_freq.QuadPart > 0 && QueryPerformanceCounter(&now))
    return (double)(now.QuadPart - g_start.QuadPart) * 1000.0 /
           (double)g_freq.QuadPart;
  return (double)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)(ts.tv_sec - g_start.tv_sec) * 1000.0 +
         (double)(ts.tv_nsec - g_start.tv_nsec) / 1e6;
#endif
}

/* getRandomValue: OS entropy where available, C11 rand() as fallback. */
static u32 random_u32(void) {
#if defined(__linux__)
  u32 v;
  ssize_t n = getrandom(&v, sizeof v, 0);
  if (n == (ssize_t)sizeof v) return v;
#endif
  static int seeded;
  if (!seeded) {
    seeded = 1;
    srand((unsigned)now_ms());
  }
  return ((u32)rand() << 16) ^ (u32)rand();
}

/* --------------------------------------------------------------- decoding -- */

/* The signature is a C string in guest memory.  As in yt() in the JS glue,
 * integers and pointers advance 4 bytes; i64/f64 advance 8 and keep the
 * cursor 8-byte aligned. */
static int decode(u32 sigp, u32 buf, Arg* out) {
  const char* sig = guest_str(sigp);
  int n = 0;
  u32 off = 0;
  for (const char* c = sig; *c && n < ARG_MAX; ++c) {
    int wide = (*c == A_I64 || *c == A_F64);
    if (wide && (off & 7)) off += 4;
    Arg a;
    memset(&a, 0, sizeof a);
    a.kind = *c;
    u64 addr = (u64)buf + off;
    const unsigned char* p =
        addr <= UINT32_MAX ? guest_ptr((u32)addr, wide ? 8 : 4) : NULL;
    if (p) {
      switch (*c) {
        case A_I32: memcpy(&a.v.i, p, 4); break;
        case A_F32: {
          float f;
          memcpy(&f, p, sizeof f);
          a.v.d = (double)f;
          break;
        }
        case A_F64: memcpy(&a.v.d, p, 8); break;
        case A_I64: memcpy(&a.v.j, p, 8); break;
        default:    memcpy(&a.v.p, p, 4); break;
      }
    }
    out[n++] = a;
    off += wide ? 8 : 4;
  }
  return n;
}

static double arg_number(const Arg* a) {
  switch (a->kind) {
    case A_F32:
    case A_F64: return a->v.d;
    case A_I64: return (double)a->v.j;
    default:    return (double)(s32)a->v.i;
  }
}

static int arg_int(const Arg* a) { return (int)(s32)a->v.i; }

static const char* arg_string(const Arg* a) { return guest_str(a->v.p); }

static void emit(const char* name, const Arg* a, int n) {
  fprintf(stderr, "[asm] %s(", name);
  for (int i = 0; i < n; ++i) {
    if (a[i].kind == A_PTR) {
      fprintf(stderr, "\"%s\"", arg_string(&a[i]));
    } else if (a[i].kind == A_I64 || a[i].kind == A_F64 ||
               a[i].kind == A_F32) {
      fprintf(stderr, "%.17g", arg_number(&a[i]));
    } else {
      fprintf(stderr, "%d", arg_int(&a[i]));
    }
    if (i + 1 < n) fprintf(stderr, ", ");
  }
  fprintf(stderr, ")\n");
}

#define SEEN_MAX 128
static u32 g_seen[SEEN_MAX];
static int g_seen_count;

static void unknown(u32 key, int argc) {
  for (int i = 0; i < g_seen_count; ++i)
    if (g_seen[i] == key) return;
  if (g_seen_count < SEEN_MAX) g_seen[g_seen_count++] = key;
  fprintf(stderr, "[asmc] key=%u argc=%d\n", key, argc);
}

/* ----------------------------------------------------------------- dispatch -- */

static double eval(u32 key, u32 sigp, u32 argbuf) {
  Arg a[ARG_MAX];
  int n = argbuf ? decode(sigp, argbuf, a) : 0;

  switch (key) {
    case 8667121: return now_ms();                  /* performance.now() */
    case 8667344: return 0.0;                       /* wasmPreInitTime */
    case 8667510: return 0.0;                       /* navigator.webdriver */
    case 8667550:                                   /* main() entered */
      fprintf(stderr, "[asm] main entered\n");
      return 0.0;
    case 8687293: return 0.0;                       /* HaveOffsetConverter */
    case 8687350: return (double)random_u32();      /* getRandomValue */
    case 8687386:                                   /* install random helper */
      emit("random_init", a, n);
      return 0.0;

    /* engine -> page hooks */
    case 8667715: {                                 /* loading status */
      fprintf(stderr, "[asm] loading %s source=%d msg=\"%s\" err=%d status=%d\n",
              (n > 0 && a[0].v.i) ? "FAILURE" : "status",
              n > 1 ? arg_int(&a[1]) : -1,
              n > 2 ? guest_str(a[2].v.p) : "",
              n > 3 ? arg_int(&a[3]) : -1,
              n > 4 ? arg_int(&a[4]) : -1);
      return 0.0;
    }
    case 8667386:                                   /* join failed */
      fprintf(stderr, "[asm] JOIN FAILED statusCode=%d\n",
              n ? arg_int(&a[0]) : -1);
      return 0.0;
    case 8666895:                                   /* teleport failed */
      fprintf(stderr, "[asm] TELEPORT FAILED statusCode=%d\n",
              n ? arg_int(&a[0]) : -1);
      return 0.0;
    case 8679911:                                   /* crash report hook */
      fprintf(stderr, "[asm] CRASH type=\"%s\" msg=\"%s\"\n",
              n > 0 ? guest_str(a[0].v.p) : "",
              n > 1 ? guest_str(a[1].v.p) : "");
      return 0.0;
    case 8667151:                                   /* onExperienceJoined */
      fprintf(stderr, "[asm] EXPERIENCE JOINED\n");
      return 0.0;
    case 8667221:                                   /* onExperienceLeft */
      fprintf(stderr, "[asm] EXPERIENCE LEFT\n");
      return 0.0;
    case 8666972:                                   /* loading screen removed */
      fprintf(stderr, "[asm] loading screen removed\n");
      return 0.0;
    case 8667459:
    case 8667064:
    case 8667287:                                   /* leave / native exit */
      fprintf(stderr, "[asm] exit (%u)\n", key);
      return 0.0;
    case 8686787:                                   /* CORS header warning */
      fprintf(stderr, "[asm] cors: non-allowlisted header %s\n",
              n ? guest_str(a[0].v.p) : "");
      return 0.0;

    /* FMOD: the mixer lives in an AudioWorklet we do not have; the driver
     * only needs a plausible sample rate and "no worklet". */
    case 8683155:
    case 8686439:
    case 8680027: return 48000.0;
    case 8680489:
    case 8680778:
    case 8681161: return 0.0;
    case 8681223:
    case 8681495:
    case 8681910:
    case 8682303:
    case 8682589:
    case 8682631:
    case 8683356:
    case 8683776:
    case 8683994:
    case 8684412:
    case 8685416:
    case 8685627:
    case 8685650:
    case 8685888:
    case 8686089:
    case 8686142:
    case 8686417:
    case 8686645: return 0.0;

    /* webview / tab prompts and page-only settings: no observable effect. */
    case 8668001:
    case 8670980:
    case 8671094:
    case 8671524:
    case 8673319:
    case 8674510:
    case 8678994:
    case 8679585:
    case 8686709:
    case 8686884: return 0.0;

    default:
      unknown(key, n);
      return 0.0;
  }
}

u32 w2c_env_emscripten_asm_const_int(struct w2c_env* env, u32 key, u32 sig,
                                     u32 buf) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_emscripten_asm_const_int", (u64)(uintptr_t)env, (u64)key, (u64)sig, (u64)buf);
  (void)env;
  return (u32)eval(key, sig, buf);
}

f64 w2c_env_emscripten_asm_const_double(struct w2c_env* env, u32 key, u32 sig,
                                        u32 buf) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_emscripten_asm_const_double", (u64)(uintptr_t)env, (u64)key, (u64)sig, (u64)buf);
  (void)env;
  return eval(key, sig, buf);
}

u32 w2c_env_emscripten_asm_const_int_sync_on_main_thread(struct w2c_env* env,
                                                         u32 key, u32 sig,
                                                         u32 buf) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_emscripten_asm_const_int_sync_on_main_thread", (u64)(uintptr_t)env, (u64)key, (u64)sig, (u64)buf);
  return w2c_env_emscripten_asm_const_int(env, key, sig, buf);
}

void w2c_env_emscripten_asm_const_async_on_main_thread(struct w2c_env* env,
                                                       u32 key, u32 sig,
                                                       u32 buf) {
  WEBLOX_IMPORT_TRACE_CALL(
      "w2c_env_emscripten_asm_const_async_on_main_thread", (u64)(uintptr_t)env, (u64)key, (u64)sig, (u64)buf);
  (void)w2c_env_emscripten_asm_const_int(env, key, sig, buf);
}
