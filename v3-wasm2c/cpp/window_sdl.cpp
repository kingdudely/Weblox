// SDL3 implementation of the window abstraction.  Compile this file instead
// of window.cpp with -DWEBLOX_USE_SDL and link SDL3.
#ifdef WEBLOX_USE_SDL

#include "window.h"

#include <SDL3/SDL.h>

namespace weblox {
namespace {
SDL_Window* g_window = nullptr;
}

bool Window::init() {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  g_window =
      SDL_CreateWindow("Weblox", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  return g_window != nullptr;
}

void Window::pump() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) break;
  }
}

void Window::quit() {
  if (g_window) {
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
  }
  SDL_Quit();
}

}  // namespace weblox

#endif  // WEBLOX_USE_SDL
