// SDL3 window/input layer for the clean C11 host.
//
// The host owns no C++ objects: this header exposes plain C functions that
// main.c can call around engine instantiation and its main loop.  The GL ES
// context is created here; GL entry points are loaded by gl.c through the
// loader returned by weblox_window_gl_loader().
#ifndef WEBLOX_WINDOW_H
#define WEBLOX_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

typedef void* (*weblox_gl_proc_fn)(const char* name);

// Create the SDL window and a GLES 3 context, make it current and load GL.
bool weblox_window_init(int width, int height);

// Drain SDL events, dispatch them to the input imports the engine registered,
// and pump the WebTransport event loop.  Returns false on SDL_EVENT_QUIT.
bool weblox_window_pump(void);

// Swap the SDL GL buffers (present the frame the engine just rendered).
void weblox_window_present(void);

// Current drawable size, in pixels.
void weblox_window_size(int* width, int* height);

// True after the user requested a quit.
bool weblox_window_should_close(void);

// Release GL, the window and SDL.
void weblox_window_shutdown(void);

// SDL_GL_GetProcAddress, used by gl.c to populate the GLAD/GLES dispatch table.
weblox_gl_proc_fn weblox_window_gl_loader(void);

#endif
