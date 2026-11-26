/* impl/wasi.c - the five wasi_snapshot_preview1 imports the module uses. */
#include "../host/host.h"
#include <sys/random.h>

u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1* a0,
                                               u32 a1, u64 a2, u32 a3) {
  (void)a0; (void)a1; (void)a2;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  *(u64*)wc_ptr(a3) = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
  return 0;
}

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1* a0,
                                                   u32 a1, u32 a2) {
  (void)a0; (void)a1; (void)a2;
  if (a1) *(u32*)wc_ptr(a1) = 0;
  if (a2) *(u32*)wc_ptr(a2) = 0;
  return 0;
}

u32 w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1* a0,
                                             u32 a1, u32 a2) {
  (void)a0; (void)a1; (void)a2;
  return 0;
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1* a0, u32 a1) {
  (void)a0;
  fprintf(stderr, "[exit] %u\n", a1);
  exit((int)a1);
}

u32 w2c_wasi__snapshot__preview1_random_get(struct w2c_wasi__snapshot__preview1* a0,
                                            u32 a1, u32 a2) {
  (void)a0;
  if (a2 && getrandom(wc_ptr(a1), a2, 0) != (ssize_t)a2) return 1;
  return 0;
}
