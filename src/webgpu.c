// Dawn-backed WebGPU facade.
//
// WEBLOX_HAVE_DAWN is defined by cmake/Dependencies.cmake only when a real
// webgpu_dawn library is linked.  The pinned WC.h has no wgpu* imports, so the
// generic webgpu_dispatch() API is kept for future generated imports: known
// names are forwarded to the Dawn C API, unknown names log once and return 0.
// Without the define every call is inert (the original no-op logging path).
#include "host.h"
#include "webgpu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(WEBLOX_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#if defined(WEBLOX_HAVE_DAWN)
#include <webgpu/webgpu.h>
#endif

static int g_ready;
static void* g_surface;
#if defined(WEBLOX_HAVE_DAWN)
static WGPUInstance g_instance;
static WGPUAdapter g_adapter;
static WGPUDevice g_device;
static volatile int g_adapter_done, g_device_done;
static int g_adapter_status = -1, g_device_status = -1;
#endif

static void gpu_log_once(const char* what) {
  static char seen[128][96];
  static unsigned n;
  for (unsigned i = 0; i < n; ++i)
    if (!strcmp(seen[i], what)) return;
  if (n < 128) {
    snprintf(seen[n], sizeof seen[n], "%s", what);
    ++n;
  }
  fprintf(stderr, "[gpu] %s not implemented\n", what);
}

static void gpu_wait_yield(void) {
#if defined(WEBLOX_HAVE_SDL3)
  SDL_Delay(1);
#else
  /* No platform sleep header is pulled in here; a short volatile spin keeps
   * this file portable and is only used during one-time device discovery. */
  for (volatile unsigned i = 0; i < 2000u; ++i) { }
#endif
}

static uint64_t gpu_ticks_ms(void) {
#if defined(WEBLOX_HAVE_SDL3)
  return SDL_GetTicks();
#else
  return (uint64_t)clock();
#endif
}

#if defined(WEBLOX_HAVE_DAWN)
static void on_adapter(WGPUAdapter adapter, WGPURequestAdapterStatus status,
                       const char* message, void* userdata) {
  (void)userdata;
  g_adapter = adapter;
  g_adapter_status = (int)status;
  if (message && *message) fprintf(stderr, "[gpu] adapter: %s\n", message);
  g_adapter_done = 1;
}

static void on_device(WGPUDevice device, WGPURequestDeviceStatus status,
                      const char* message, void* userdata) {
  (void)userdata;
  g_device = device;
  g_device_status = (int)status;
  if (message && *message) fprintf(stderr, "[gpu] device: %s\n", message);
  g_device_done = 1;
}

static int wait_flag(volatile int* flag) {
  uint64_t start = gpu_ticks_ms();
  while (!*flag && gpu_ticks_ms() - start < 5000u) {
    wgpuInstanceProcessEvents(g_instance);
    gpu_wait_yield();
  }
  return *flag != 0;
}
#endif

bool webgpu_init(void) {
  if (g_ready) return true;
#if defined(WEBLOX_HAVE_DAWN)
  if (!g_instance) {
    WGPUInstanceDescriptor desc;
    memset(&desc, 0, sizeof desc);
    g_instance = wgpuCreateInstance(&desc);
    if (!g_instance) {
      fprintf(stderr, "[gpu] wgpuCreateInstance failed\n");
      return false;
    }
  }
  g_adapter_done = 0;
  wgpuInstanceRequestAdapter(g_instance, NULL, on_adapter, NULL);
  if (!wait_flag(&g_adapter_done) ||
      g_adapter_status != WGPURequestAdapterStatus_Success || !g_adapter) {
    fprintf(stderr, "[gpu] no Dawn adapter (status=%d)\n", g_adapter_status);
    return false;
  }
  g_device_done = 0;
  wgpuAdapterRequestDevice(g_adapter, NULL, on_device, NULL);
  if (!wait_flag(&g_device_done) ||
      g_device_status != WGPURequestDeviceStatus_Success || !g_device) {
    fprintf(stderr, "[gpu] no Dawn device (status=%d)\n", g_device_status);
    return false;
  }
  g_ready = 1;
  fprintf(stderr, "[gpu] Dawn instance/adapter/device ready\n");
  return true;
#else
  gpu_log_once("webgpu_init (Dawn not compiled in)");
  return false;
#endif
}

bool webgpu_ready(void) { return g_ready != 0; }

void* webgpu_instance_handle(void) {
#if defined(WEBLOX_HAVE_DAWN)
  return (void*)g_instance;
#else
  return NULL;
#endif
}

bool webgpu_create_surface(void* native_window, void* native_display) {
#if defined(WEBLOX_HAVE_DAWN) && defined(WEBLOX_HAVE_DAWN_SURFACE)
  if (!g_instance || !native_window) return false;
  WGPUSurfaceDescriptor desc;
  memset(&desc, 0, sizeof desc);
#if defined(_WIN32)
  WGPUSurfaceDescriptorFromWindowsHWND win;
  memset(&win, 0, sizeof win);
  win.chain.sType = WGPUSType_SurfaceDescriptorFromWindowsHWND;
  win.hwnd = native_window;
  win.hinstance = NULL;
  desc.nextInChain = (const WGPUChainedStruct*)&win;
#elif defined(__APPLE__)
  WGPUSurfaceDescriptorFromMetalLayer metal;
  memset(&metal, 0, sizeof metal);
  metal.chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer;
  metal.layer = native_window;
  desc.nextInChain = (const WGPUChainedStruct*)&metal;
#elif defined(__linux__)
  WGPUSurfaceDescriptorFromXlibWindow x11;
  memset(&x11, 0, sizeof x11);
  x11.chain.sType = WGPUSType_SurfaceDescriptorFromXlibWindow;
  x11.display = native_display;
  x11.window = (uint32_t)(uintptr_t)native_window;
  desc.nextInChain = (const WGPUChainedStruct*)&x11;
#else
  (void)native_display;
  gpu_log_once("webgpu_create_surface (platform descriptor unavailable)");
  return false;
#endif
  WGPUSurface surface = wgpuInstanceCreateSurface(g_instance, &desc);
  if (!surface) {
    fprintf(stderr, "[gpu] wgpuInstanceCreateSurface failed\n");
    return false;
  }
  g_surface = (void*)surface;
  fprintf(stderr, "[gpu] Dawn surface created\n");
  return true;
#else
  (void)native_window; (void)native_display;
  gpu_log_once("webgpu_create_surface (Dawn surface support not compiled in)");
  return false;
#endif
}

void* webgpu_surface_handle(void) { return g_surface; }

/* Raw-ABI dispatch.  Scalar arguments are passed through; pointer arguments
 * are u32 handles from the wasm ABI, cast with uintptr_t. */
#define GPU_CALL0(name, fn)                                                    \
  if (!strcmp(n, name)) {                                                       \
    if (!g_device) { gpu_log_once(name " (no device)"); return 0; }              \
    fn();                                                                       \
    return 0;                                                                   \
  }
#define GPU_DEV(name, fn, type)                                                \
  if (!strcmp(n, name)) {                                                       \
    if (!g_device || argc < 2) { gpu_log_once(name " (bad args)"); return 0; }   \
    return (u32)(uintptr_t)fn(g_device, (type)(uintptr_t)args[1]);              \
  }
#define GPU_VOID1(name, fn, type)                                              \
  if (!strcmp(n, name)) {                                                       \
    if (argc < 2) { gpu_log_once(name " (bad args)"); return 0; }                \
    fn((type)(uintptr_t)args[1]);                                                \
    return 0;                                                                    \
  }

u32 webgpu_dispatch(const char* name, const u32* args, u32 argc) {
  if (!name || !*name) return 0;
#if defined(WEBLOX_HAVE_DAWN)
  const char* n = name;
  if (!strncmp(n, "wgpu", 4) && n[4]) n += 4; /* accept wgpuFoo and Foo */
  if (!strcmp(n, "InstanceProcessEvents")) {
    if (g_instance) wgpuInstanceProcessEvents(g_instance);
    return 0;
  }
  if (!strcmp(n, "InstanceCreateSurface")) {
    if (!g_instance || argc < 2) return 0;
    return (u32)(uintptr_t)wgpuInstanceCreateSurface(
        g_instance, (const WGPUSurfaceDescriptor*)(uintptr_t)args[1]);
  }
  if (!strcmp(n, "DeviceGetQueue")) {
    if (!g_device) { gpu_log_once("DeviceGetQueue (no device)"); return 0; }
    return (u32)(uintptr_t)wgpuDeviceGetQueue(g_device);
  }
  GPU_DEV("DeviceCreateBuffer", wgpuDeviceCreateBuffer, const WGPUBufferDescriptor*)
  GPU_DEV("DeviceCreateTexture", wgpuDeviceCreateTexture, const WGPUTextureDescriptor*)
  GPU_DEV("DeviceCreateShaderModule", wgpuDeviceCreateShaderModule,
          const WGPUShaderModuleDescriptor*)
  GPU_DEV("DeviceCreateCommandEncoder", wgpuDeviceCreateCommandEncoder,
          const WGPUCommandEncoderDescriptor*)
  GPU_DEV("DeviceCreateBindGroupLayout", wgpuDeviceCreateBindGroupLayout,
          const WGPUBindGroupLayoutDescriptor*)
  GPU_DEV("DeviceCreatePipelineLayout", wgpuDeviceCreatePipelineLayout,
          const WGPUPipelineLayoutDescriptor*)
  GPU_DEV("DeviceCreateRenderPipeline", wgpuDeviceCreateRenderPipeline,
          const WGPURenderPipelineDescriptor*)
  GPU_VOID1("BufferRelease", wgpuBufferRelease, WGPUBuffer)
  GPU_VOID1("TextureRelease", wgpuTextureRelease, WGPUTexture)
  GPU_VOID1("ShaderModuleRelease", wgpuShaderModuleRelease, WGPUShaderModule)
  GPU_VOID1("CommandEncoderRelease", wgpuCommandEncoderRelease, WGPUCommandEncoder)
  GPU_VOID1("QueueRelease", wgpuQueueRelease, WGPUQueue)
  if (!strcmp(n, "QueueSubmit")) {
    if (!g_device || argc < 4) { gpu_log_once("QueueSubmit (bad args)"); return 0; }
    wgpuQueueSubmit((WGPUQueue)(uintptr_t)args[1], args[2],
                    (const WGPUCommandBuffer* const*)(uintptr_t)args[3]);
    return 0;
  }
  if (!strcmp(n, "DeviceRelease")) {
    if (g_device) { wgpuDeviceRelease(g_device); g_device = NULL; }
    g_ready = 0;
    return 0;
  }
#else
  (void)args; (void)argc;
#endif
  gpu_log_once(name);
  return 0;
}

void webgpu_shutdown(void) {
#if defined(WEBLOX_HAVE_DAWN)
  if (g_surface) { wgpuSurfaceRelease((WGPUSurface)g_surface); g_surface = NULL; }
  if (g_device) { wgpuDeviceRelease(g_device); g_device = NULL; }
  if (g_adapter) { wgpuAdapterRelease(g_adapter); g_adapter = NULL; }
  if (g_instance) { wgpuInstanceRelease(g_instance); g_instance = NULL; }
#else
  gpu_log_once("webgpu_shutdown (Dawn not compiled in)");
#endif
  g_ready = 0;
}
