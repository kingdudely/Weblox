// Default no-op GPU backend.  Replace with gpu_gl.cpp / gpu_webgpu.cpp when
// those implementations land; the rest of the host never changes.
#include "gpu.h"

namespace weblox::gpu {

bool init(Backend, const Surface&) { return false; }
void present() {}
void shutdown() {}

}  // namespace weblox::gpu
