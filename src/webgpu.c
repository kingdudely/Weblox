// Dawn-backed WebGPU facade.
//
// Build with WEBLOX_HAVE_DAWN=1 and include Dawn's `webgpu/webgpu.h` plus the
// dawn_proc (and, for a local device, dawn_native/dawn_platform) libraries.
// Without that define the layer is inert: every call logs once and returns 0,
// which keeps the C host buildable on machines without Dawn installed.
#include "host.h"
#include "webgpu.h"

#include <stdio.h>
#include <string.h>

#if defined(WEBLOX_HAVE_DAWN)
#include <webgpu/webgpu.h>
#endif

static int g_ready;
#if defined(WEBLOX_HAVE_DAWN)
static WGPUInstance g_instance;
static WGPUAdapter g_adapter;
static WGPUDevice g_device;
#endif

static void gpu_log_once(const char* what) {
  static char seen[128][64];
  static unsigned n;
  for (unsigned i = 0; i < n; ++i)
    if (!strcmp(seen[i], what)) return;
  if (n < 128) {
    snprintf(seen[n], sizeof seen[n], "%s", what);
    ++n;
  }
  fprintf(stderr, "[gpu] %s not implemented\n", what);
}

bool webgpu_init(void) {
  if (g_ready) return true;
#if defined(WEBLOX_HAVE_DAWN)
  WGPUInstanceDescriptor desc;
  memset(&desc, 0, sizeof desc);
  g_instance = wgpuCreateInstance(&desc);
  if (!g_instance) {
    fprintf(stderr, "[gpu] wgpuCreateInstance failed\n");
    return false;
  }
  /* TODO: wgpuInstanceRequestAdapter with a callback that populates g_adapter
   * and wgpuAdapterRequestDevice for g_device.  The engine does not import any
   * WebGPU entry points in the pinned WC.h, so no surfaces are needed yet. */
  g_ready = 1;
  fprintf(stderr, "[gpu] Dawn instance created (adapter/device pending)\n");
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

u32 webgpu_dispatch(const char* name, const u32* args, u32 argc) {
  (void)args;
  (void)argc;
  if (!name || !*name) return 0;
#if defined(WEBLOX_HAVE_DAWN)
  /* Generated WebGPU imports will be added here as they appear in WC.h.
   * Example mapping: "wgpuDeviceCreateShaderModule" -> wgpuDeviceCreateShaderModule(
   *     g_device, *(const WGPUShaderModuleDescriptor*)(uintptr_t)args[1]). */
#endif
  gpu_log_once(name);
  return 0;
}

void webgpu_shutdown(void) {
#if defined(WEBLOX_HAVE_DAWN)
  if (g_device) wgpuDeviceRelease(g_device);
  if (g_adapter) wgpuAdapterRelease(g_adapter);
  if (g_instance) wgpuInstanceRelease(g_instance);
  g_device = NULL;
  g_adapter = NULL;
  g_instance = NULL;
#else
  gpu_log_once("webgpu_shutdown (Dawn not compiled in)");
#endif
  g_ready = 0;
}
