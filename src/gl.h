// WebGL/GLES import layer.  SDL3 creates the context; GLAD (GLES2 profile)
// loads and owns the function pointers when available.  A proc-address table
// populated from SDL_GL_GetProcAddress is the fallback when GLAD is not built.
#ifndef WEBLOX_GL_H
#define WEBLOX_GL_H

#include "host.h"
#include "window.h"

#include <stdbool.h>

#if defined(WEBLOX_HAVE_GLAD_GLES2)
#include <glad/gles2.h>
#endif

bool gl_init(weblox_gl_proc_fn loader);
bool gl_ready(void);
void gl_shutdown(void);

// Call once per frame after the engine has drawn (no-op: GL is immediate).
void gl_flush(void);

#endif
