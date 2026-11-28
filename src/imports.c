// Core imports implemented directly in C.  Every function name and signature
// comes from the generated WC.h; gen_stubs.py skips the names in
// tools/implemented.txt so there is exactly one definition of each.

#include "host.h"
#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

void w2c_env_emscripten_console_log(struct w2c_env* env, u32 ptr) {
  (void)env;
  fprintf(stderr, "[log ] %s\n", guest_str(ptr));
}

void w2c_env_emscripten_console_warn(struct w2c_env* env, u32 ptr) {
  (void)env;
  fprintf(stderr, "[warn] %s\n", guest_str(ptr));
}

void w2c_env_emscripten_console_error(struct w2c_env* env, u32 ptr) {
  (void)env;
  fprintf(stderr, "[err ] %s\n", guest_str(ptr));
}

f64 w2c_env_emscripten_get_now(struct w2c_env* env) {
  (void)env;
  // performance.now() is monotonic time since process start, not CPU time.
  static long long start_ms;
  long long now_ms;
#if defined(_WIN32)
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  now_ms = (long long)(counter.QuadPart * 1000 / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
  if (!start_ms) start_ms = now_ms;
  return (f64)(now_ms - start_ms);
}

f64 w2c_env_emscripten_date_now(struct w2c_env* env) {
  (void)env;
  return (f64)time(NULL) * 1000.0;
}

u32 w2c_wasi__snapshot__preview1_random_get(
    struct w2c_wasi__snapshot__preview1* env, u32 ptr, u32 len) {
  (void)env;
  unsigned char* base = (unsigned char*)weblox_mem_base();
  if (!base || (u64)ptr + len > weblox_mem_size()) return 1;
  for (u32 i = 0; i < len; ++i) base[ptr + i] = (unsigned char)rand();
  return 0;
}

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(
    struct w2c_wasi__snapshot__preview1* env, u32 count_ptr, u32 size_ptr) {
  (void)env;
  guest_put_u32(count_ptr, 0);
  guest_put_u32(size_ptr, 0);
  return 0;
}

u32 w2c_wasi__snapshot__preview1_environ_get(
    struct w2c_wasi__snapshot__preview1* env, u32 environ_ptr, u32 buf_ptr) {
  (void)env; (void)environ_ptr; (void)buf_ptr;
  return 0;
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(
    struct w2c_wasi__snapshot__preview1* env, u32 clock_id, u64 precision,
    u32 out_ptr) {
  (void)env; (void)precision;
  u64 ns;
  if (clock_id == 0) {
    ns = (u64)time(NULL) * 1000000000ull;
  } else {
    ns = (u64)clock() * 1000000000ull / CLOCKS_PER_SEC;
  }
  guest_put_u64(out_ptr, ns);
  return 0;
}

void w2c_wasi__snapshot__preview1_proc_exit(
    struct w2c_wasi__snapshot__preview1* env, u32 code) {
  (void)env;
  exit((int)code);
}
