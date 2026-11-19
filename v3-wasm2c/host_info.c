/* impl/host_info.c - host identity, canvas and device queries.
 *
 * Values mirror what WasmPlayer.js puts in `hostInfo`: a Linux desktop, no
 * touch, a fixed 1280x720 canvas and dpr 1.0.  Keys are C strings.
 */
#include "../host/host.h"

#define CANVAS_W 1280
#define CANVAS_H 720

static int nproc_cached;

static int nproc_get(void) {
  if (!nproc_cached) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    nproc_cached = n > 0 ? (int)n : 1;
  }
  return nproc_cached;
}

static const char* info_str(const char* key) {
  if (!key) return "";
  if (!strcmp(key, "hostOS")) return "Linux";
  if (!strcmp(key, "osName")) return "Linux";
  if (!strcmp(key, "osVersion")) return "";
  if (!strcmp(key, "deviceModel")) return "";
  if (!strcmp(key, "browserName")) return "Chrome";
  if (!strcmp(key, "browserVersion")) return "140.0.0.0";
  if (!strcmp(key, "gpuVendor")) return "Mesa";
  if (!strcmp(key, "gpuRenderer")) return "llvmpipe";
  if (!strcmp(key, "screenWidthDip")) return "1280";
  if (!strcmp(key, "screenHeightDip")) return "720";
  if (!strcmp(key, "screenWidthPx")) return "1280";
  if (!strcmp(key, "screenHeightPx")) return "720";
  if (!strcmp(key, "dpi")) return "96";
  if (!strcmp(key, "mobile")) return "0";
  if (!strcmp(key, "memoryMB")) return "2048";
  if (!strcmp(key, "cpuLogicalCount")) return "1";
  /* unknown key: log once per distinct key is overkill, keep it quiet */
  return "";
}

int wc_host_info_int_key(const char* key) {
  if (!key) return 0;
  if (!strcmp(key, "hostOS")) return 5;          /* Linux */
  if (!strcmp(key, "mobile")) return 0;
  if (!strcmp(key, "screenWidthDip")) return CANVAS_W;
  if (!strcmp(key, "screenHeightDip")) return CANVAS_H;
  if (!strcmp(key, "screenWidthPx")) return CANVAS_W;
  if (!strcmp(key, "screenHeightPx")) return CANVAS_H;
  if (!strcmp(key, "dpi")) return 96;
  if (!strcmp(key, "memoryMB")) return 2048;
  if (!strcmp(key, "cpuLogicalCount")) return nproc_get();
  return atoi(info_str(key));
}

u32 wc_impl_read_host_info_int(struct w2c_env* a0, u32 a1) {
  (void)a0;
  return (u32)wc_host_info_int_key(wc_str(a1));
}

void wc_impl_read_host_info_string(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0;
  const char* v = info_str(wc_str(a1));
  if (!a3) return;
  size_t n = strlen(v);
  if (n > a3 - 1) n = a3 - 1;
  memcpy(wc_ptr(a2), v, n);
  ((char*)wc_ptr(a2))[n] = 0;
}

u32 wc_impl_get_hardware_concurrency(struct w2c_env* a0) { (void)a0; return (u32)nproc_get(); }
u32 wc_impl_emscripten_num_logical_cores(struct w2c_env* a0) { (void)a0; return (u32)nproc_get(); }
u32 wc_impl_emscripten_get_heap_max(struct w2c_env* a0) { (void)a0; return 0xFFFFFFFFu;   /* 4 GiB does not fit in u32; callers use it as an upper bound */ }

/* The memory is allocated at the module maximum (4 GiB) and never grows, so a
 * request is satisfied whenever it is inside it.  dlmalloc's own check
 * overflows (`memory.pages << 16` is 0 for 65536 pages) and would call this on
 * every sbrk, so returning 1 is what keeps the heap working. */
u32 wc_impl_emscripten_resize_heap(struct w2c_env* a0, u32 want) {
  (void)a0;
  return (u64)want <= g_mem.size;
}

f64 wc_impl_get_effective_device_pixel_ratio(struct w2c_env* a0) { (void)a0; return 1.0; }
f64 wc_impl_emscripten_get_device_pixel_ratio(struct w2c_env* a0) { (void)a0; return 1.0; }

u32 wc_impl_has_keyboard(struct w2c_env* a0) { (void)a0; return 1; }
u32 wc_impl_has_mouse(struct w2c_env* a0) { (void)a0; return 1; }
u32 wc_impl_is_mobile_browser(struct w2c_env* a0) { (void)a0; return 0; }
u32 wc_impl_is_touch_mobile(struct w2c_env* a0) { (void)a0; return 0; }
u32 wc_impl_is_touch_screen_device(struct w2c_env* a0) { (void)a0; return 0; }

u32 wc_impl_emscripten_get_canvas_element_size(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0; (void)a1;
  if (a2) *(u32*)wc_ptr(a2) = CANVAS_W;
  if (a3) *(u32*)wc_ptr(a3) = CANVAS_H;
  return 1;
}

u32 wc_impl_emscripten_set_canvas_element_size(struct w2c_env* a0, u32 a1, u32 a2, u32 a3) {
  (void)a0; (void)a1; (void)a2; (void)a3;
  return 1;
}

u32 wc_impl_applyPendingCanvasResize(struct w2c_env* a0, u32 a1, u32 a2) {
  (void)a0; (void)a1; (void)a2;
  return 0;
}

void wc_impl_shift_canvas(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }

/* ---------------- page/device booleans ---------------- */

u32 wc_impl_js_web_client_page_visible(struct w2c_env* a0) { (void)a0; return 1; }
void wc_impl_report_crash(struct w2c_env* a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) {
  (void)a0; (void)a2; (void)a3; (void)a5;
  fprintf(stderr, "[crash] %s / %s / %s\n", wc_str(a1), wc_str(a2), wc_str(a4));
}
void wc_impl_set_crash_attribute(struct w2c_env* a0, u32 a1, u32 a2) { (void)a0; (void)a1; (void)a2; }
void wc_impl_preallocate_emscripten_key_event_buffer(struct w2c_env* a0, u32 a1) { (void)a0; (void)a1; }
void wc_impl_install_pointer_source_tracking(struct w2c_env* a0) { (void)a0; }
