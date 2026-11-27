/* Host identity, canvas and device queries, ported from
 * v3-wasm2c/impl/host_info.c.  Every signature comes from WC.h.
 *
 * Values mirror what WasmPlayer.js reports for a Linux desktop Chrome 140:
 * no touch, keyboard and mouse present, a 1280x720 canvas and dpr 1.0.  The
 * canvas/dpr/keyboard/mouse imports already live in window_sdl.c (they use the
 * SDL3 window when available); this file implements the remaining identity,
 * capability and crash imports that otherwise resolve to no-op stubs.
 */
#include "host.h"
#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(WEBLOX_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#define WEBLOX_CANVAS_W 1280
#define WEBLOX_CANVAS_H 720

static int cpu_count(void) {
  static int cached;
  if (cached) return cached;
  const char* override = getenv("WEBLOX_CPU_COUNT");
  if (override && *override) {
    long n = strtol(override, NULL, 10);
    cached = n > 0 ? (int)n : 1;
  } else {
#if defined(WEBLOX_HAVE_SDL3)
    cached = SDL_GetNumLogicalCPUCores();
    if (cached <= 0) cached = 1;
#else
    cached = 1;  // C11 has no portable processor-count query
#endif
  }
  return cached;
}

static const char* info_string(const char* key) {
  if (!key) return "";
  if (!strcmp(key, "hostOS") || !strcmp(key, "osName") ||
      !strcmp(key, "platform"))
    return "Linux";
  if (!strcmp(key, "osVersion")) return "";
  if (!strcmp(key, "deviceModel")) return "";
  if (!strcmp(key, "deviceType")) return "desktop";
  if (!strcmp(key, "browserName")) return "Chrome";
  if (!strcmp(key, "browserVersion")) return "140.0.0.0";
  if (!strcmp(key, "gpuVendor")) return "Mesa";
  if (!strcmp(key, "gpuRenderer")) return "llvmpipe";
  if (!strcmp(key, "screenWidthDip") || !strcmp(key, "screenWidthPx"))
    return "1280";
  if (!strcmp(key, "screenHeightDip") || !strcmp(key, "screenHeightPx"))
    return "720";
  if (!strcmp(key, "dpi")) return "96";
  if (!strcmp(key, "mobile") || !strcmp(key, "isMobile")) return "0";
  if (!strcmp(key, "memoryMB")) return "2048";
  if (!strcmp(key, "cpuLogicalCount")) {
    static char buf[24];
    snprintf(buf, sizeof buf, "%d", cpu_count());
    return buf;
  }
  if (!strcmp(key, "architecture")) return "x86_64";
  if (!strcmp(key, "language")) return "en-us";
  return "";
}

static int info_int(const char* key) {
  if (!key) return 0;
  if (!strcmp(key, "hostOS") || !strcmp(key, "platform")) return 5; /* Linux */
  if (!strcmp(key, "mobile") || !strcmp(key, "isMobile")) return 0;
  if (!strcmp(key, "screenWidthDip") || !strcmp(key, "screenWidthPx"))
    return WEBLOX_CANVAS_W;
  if (!strcmp(key, "screenHeightDip") || !strcmp(key, "screenHeightPx"))
    return WEBLOX_CANVAS_H;
  if (!strcmp(key, "dpi")) return 96;
  if (!strcmp(key, "memoryMB")) return 2048;
  if (!strcmp(key, "cpuLogicalCount")) return cpu_count();
  return atoi(info_string(key));
}

u32 w2c_env_read_host_info_int(struct w2c_env* env, u32 key_ptr) {
  (void)env;
  const char* key = guest_str(key_ptr);
  int value = info_int(key);
  fprintf(stderr, "[host] read_host_info_int(%s) = %d\n", key, value);
  return (u32)value;
}

void w2c_env_read_host_info_string(struct w2c_env* env, u32 key_ptr,
                                   u32 out_ptr, u32 capacity) {
  (void)env;
  const char* key = guest_str(key_ptr);
  const char* value = info_string(key);
  if (!capacity) return;
  size_t n = strlen(value);
  if (n > (size_t)capacity - 1) n = (size_t)capacity - 1;
  unsigned char* out = guest_ptr(out_ptr, n + 1);
  if (!out) return;
  memcpy(out, value, n);
  out[n] = 0;
  fprintf(stderr, "[host] read_host_info_string(%s) = \"%s\"\n", key, value);
}

u32 w2c_env_get_hardware_concurrency(struct w2c_env* env) {
  (void)env;
  return (u32)cpu_count();
}

u32 w2c_env_emscripten_num_logical_cores(struct w2c_env* env) {
  (void)env;
  return (u32)cpu_count();
}

u32 w2c_env_emscripten_get_heap_max(struct w2c_env* env) {
  (void)env;
  // 4 GiB does not fit in the u32 return type; callers use it as a bound.
  return 0xFFFFFFFFu;
}

/* The guest memory is reserved at its module maximum and never shrinks.
 * dlmalloc's own grow check overflows at 65536 pages and calls this on every
 * sbrk, so answering 1 for any address inside the reservation is required. */
u32 w2c_env_emscripten_resize_heap(struct w2c_env* env, u32 want) {
  (void)env;
  int ok = (u64)want <= g_mem.size;
  if (!ok) fprintf(stderr, "[host] resize_heap(%u) denied\n", want);
  return ok ? 1u : 0u;
}

u32 w2c_env_is_mobile_browser(struct w2c_env* env) {
  (void)env;
  return 0;
}

u32 w2c_env_is_touch_mobile(struct w2c_env* env) {
  (void)env;
  return 0;
}

u32 w2c_env_is_touch_screen_device(struct w2c_env* env) {
  (void)env;
  return 0;
}

u32 w2c_env_is_mouse_sourced_event(struct w2c_env* env) {
  (void)env;
  return 0;
}

u32 w2c_env_js_web_client_page_visible(struct w2c_env* env) {
  (void)env;
  return 1;
}

u32 w2c_env_js_keyboard_layout_status(struct w2c_env* env) {
  (void)env;
  return 1; /* layout ready, matching v2-wasmtime's input import */
}

void w2c_env_js_copy_keyboard_layout_key(struct w2c_env* env, u32 from,
                                         u32 to, u32 count) {
  (void)env;
  (void)from;
  (void)to;
  (void)count;
}

void w2c_env_js_request_keyboard_layout(struct w2c_env* env) {
  (void)env;
  fprintf(stderr, "[host] keyboard layout request (US layout assumed)\n");
}

void w2c_env_install_pointer_source_tracking(struct w2c_env* env) {
  (void)env;
}

void w2c_env_preallocate_emscripten_key_event_buffer(struct w2c_env* env,
                                                     u32 count) {
  (void)env;
  (void)count;
}

void w2c_env_shift_canvas(struct w2c_env* env, f32 offset) {
  (void)env;
  (void)offset;
}

void w2c_env_report_crash(struct w2c_env* env, u32 a, u32 b, u32 c, u32 d,
                          u32 e) {
  (void)env;
  (void)c;
  (void)e;
  fprintf(stderr, "[host] crash: %s / %s / %s\n", guest_str(a), guest_str(b),
          guest_str(d));
}

void w2c_env_set_crash_attribute(struct w2c_env* env, u32 key_ptr,
                                 u32 value_ptr) {
  (void)env;
  fprintf(stderr, "[host] crash attribute %s=%s\n", guest_str(key_ptr),
          guest_str(value_ptr));
}
