// GPU abstraction.  The engine asks for WebGL and WebGPU; this interface is
// what those imports will call, so platform code stays out of host.cpp.
//
// Implementations to add:
//   WebGL  -> SDL_GL + GLAD (or ANGLE) behind gpu_gl.cpp
//   WebGPU -> Dawn / wgpu-native behind gpu_webgpu.cpp
#pragma once

#include <cstdint>

namespace weblox::gpu {

enum class Backend { None, WebGL, WebGPU };

struct Surface {
  void* native = nullptr;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

bool init(Backend backend, const Surface& surface);
void present();
void shutdown();

}  // namespace weblox::gpu
