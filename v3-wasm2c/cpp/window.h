// Cross-platform window/input/audio abstraction.
//
// The default implementation is a no-op so the skeleton builds anywhere.
// Define WEBLOX_USE_SDL when compiling window_sdl.cpp to get a real SDL3
// window on Linux/macOS/Windows from the same host code.
#pragma once

namespace weblox {

class Window {
 public:
  bool init();
  void pump();
  void quit();
};

}  // namespace weblox
