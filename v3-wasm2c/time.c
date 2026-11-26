/* impl/time.c - clock, console, abort/assert and tz glue.
 *
 * The guest only ever asks for wall/monotonic milliseconds and local-time
 * conversion; both map straight onto the host.
 */
#include "../host/host.h"
#include <string.h>
#include <execinfo.h>
#include <sys/time.h>

static double mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Hot spin loops in the module (wait_for wrappers) poll the clock instead of
 * suspending, so this is the natural place to hand control back: let a queued
 * fiber run, and every so often pump one unit of engine work. */
int wc_pump_once(void);
extern __thread int g_is_main;

f64 wc_impl_emscripten_get_now(struct w2c_env* a0) {
  (void)a0;
  static unsigned n;
  if ((++n & 0x3FF) == 0 && g_is_main) wc_pump_once();
  return mono_ms();
}

f64 wc_impl_emscripten_date_now(struct w2c_env* a0) {
  (void)a0;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* musl wasm32 struct tm: 9 ints, then long gmtoff, then const char* zone */
static void tm_write(u32 p, const struct tm* t, long gmtoff) {
  int* d = (int*)wc_ptr(p);
  d[0] = t->tm_sec; d[1] = t->tm_min; d[2] = t->tm_hour;
  d[3] = t->tm_mday; d[4] = t->tm_mon; d[5] = t->tm_year;
  d[6] = t->tm_wday; d[7] = t->tm_yday; d[8] = t->tm_isdst;
  *(long*)(d + 9) = gmtoff;
  *(u32*)(d + 10) = 0; /* tm_zone */
}

u32 wc_impl_0x5Fgmtime_js(struct w2c_env* a0, u64 a1, u32 a2) {
  (void)a0;
  time_t t = (time_t)a1;
  struct tm out;
  if (!gmtime_r(&t, &out)) { ((char*)wc_ptr(a2))[0] = 0; return a2; }
  tm_write(a2, &out, 0);
  return a2;
}

u32 wc_impl_0x5Flocaltime_js(struct w2c_env* a0, u64 a1, u32 a2) {
  (void)a0;
  time_t t = (time_t)a1;
  struct tm out;
  if (!localtime_r(&t, &out)) { ((char*)wc_ptr(a2))[0] = 0; return a2; }
  tm_write(a2, &out, out.tm_gmtoff);
  return a2;
}

u64 wc_impl_0x5Fmktime_js(struct w2c_env* a0, u32 a1) {
  (void)a0;
  const int* d = (const int*)wc_ptr(a1);
  struct tm in;
  memset(&in, 0, sizeof in);
  in.tm_sec = d[0]; in.tm_min = d[1]; in.tm_hour = d[2];
  in.tm_mday = d[3]; in.tm_mon = d[4]; in.tm_year = d[5];
  in.tm_isdst = d[8];
  return (u64)mktime(&in);
}

void wc_impl_0x5Ftzset_js(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4) {
  (void)a0;
  tzset();
  if (a1) *(u32*)wc_ptr(a1) = (u32)(timezone / 60);
  if (a2) *(u32*)wc_ptr(a2) = (u32)daylight;
  if (a3) strcpy((char*)wc_ptr(a3), tzname[0] ? tzname[0] : "UTC");
  if (a4) strcpy((char*)wc_ptr(a4), tzname[1] ? tzname[1] : "UTC");
}

static void write_line(u32 p, u32 n, FILE* to) {
  if (!p || !n) return;
  fwrite(wc_ptr(p), 1, n, to);
  if (!n || ((const char*)wc_ptr(p))[n - 1] != '\n') fputc('\n', to);
}

void wc_impl_emscripten_outn(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; write_line(a1, a2, stdout); }
void wc_impl_emscripten_errn(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; write_line(a1, a2, stderr); }

void wc_impl_0x5Femscripten_console_log(struct w2c_env* a0, u32 a1) {
  (void)a0;
  fprintf(stderr, "[log ] %s\n", wc_str(a1));
}
void wc_impl_0x5Femscripten_console_warn(struct w2c_env* a0, u32 a1) {
  (void)a0;
  fprintf(stderr, "[warn] %s\n", wc_str(a1));
}
void wc_impl_0x5Femscripten_console_error(struct w2c_env* a0, u32 a1) {
  (void)a0;
  fprintf(stderr, "[err ] %s\n", wc_str(a1));
}

void wc_bt_dump(void) {
  /* -fno-asynchronous-unwind-tables breaks backtrace(); the tree is built at
   * -O0, so the rbp chain is intact - walk it instead */
  void** fp = (void**)__builtin_frame_address(0);
  for (int i = 0; i < 24 && fp; i++) {
    void** next = (void**)fp[0];
    fprintf(stderr, "[bt] %2d %p\n", i, fp[1]);
    if (!next || next <= fp) break;
    fp = next;
  }
}

void wc_impl_0x5Fabort_js(struct w2c_env* a0) {
  (void)a0;
  fprintf(stderr, "[ABORT] _abort_js sp=%x base=%x end=%x\n",
          g_inst ? w2c_WebClient_get_stack_ptr(g_inst) : 0,
          g_inst ? w2c_WebClient_get_stack_base(g_inst) : 0,
          g_inst ? w2c_WebClient_get_stack_end(g_inst) : 0);
  wc_bt_dump();
}

void wc_impl_0x5F_abort_js(struct w2c_env* a0) {
  (void)a0;
  fprintf(stderr, "[ABORT] wasm called abort() sp=%x base=%x end=%x\n",
          g_inst ? w2c_WebClient_get_stack_ptr(g_inst) : 0,
          g_inst ? w2c_WebClient_get_stack_base(g_inst) : 0,
          g_inst ? w2c_WebClient_get_stack_end(g_inst) : 0);
  wc_bt_dump();
}

/* unprefixed variants used by the engine's own logging path */
extern int g_trace_calls;
void wc_impl_emscripten_console_log(struct w2c_env* a0, u32 a1) {
  (void)a0;
  const char* m = wc_str(a1);
  fprintf(stderr, "[log ] %s\n", m);
  if (m && (strstr(m, "gameLoadedSignal") || strstr(m, "core scripts") || strstr(m, "scrips are loaded"))) {
    g_trace_calls = 3000;
    fprintf(stderr, "[trace] start\n");
  }
}
void wc_impl_emscripten_console_warn(struct w2c_env* a0, u32 a1) { (void)a0; fprintf(stderr, "[warn] %s\n", wc_str(a1)); }
void wc_impl_emscripten_console_error(struct w2c_env* a0, u32 a1) { (void)a0; fprintf(stderr, "[err ] %s\n", wc_str(a1)); }
