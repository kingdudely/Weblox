// SDL3 window, GLES context and input pump for the clean C11 host.
//
// Build with WEBLOX_HAVE_SDL3=1 and link SDL3.  Without that define the file
// still compiles: every entry point logs once with a [window] prefix and the
// window layer is inert (useful for headless CI).
//
// Event structs below mirror emscripten/html5.h field-for-field.  They are
// written into guest linear memory through guest.h, and registered wasm
// callbacks are invoked through the engine's own indirect function table (via
// the exported table accessor in WC.h) - no hardcoded indices or addresses.
#include "host.h"
#include "guest.h"
#include "window.h"
#include "gl.h"
#include "webtransport.h"
#include "webgpu.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(WEBLOX_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#if defined(WEBLOX_HAVE_SDL3)
static SDL_Window* g_window;
static SDL_GLContext g_gl_context;
#endif
static int g_width = 1280, g_height = 720;
static int g_ready, g_should_close;

/* ------------------------------------------------------------------------- */
/* Registered engine callbacks                                               */
/* ------------------------------------------------------------------------- */

enum {
  CB_KEYDOWN, CB_KEYUP,
  CB_MOUSEDOWN, CB_MOUSEUP, CB_MOUSEMOVE, CB_MOUSEENTER, CB_MOUSELEAVE, CB_WHEEL,
  CB_TOUCHSTART, CB_TOUCHEND, CB_TOUCHMOVE, CB_TOUCHCANCEL,
  CB_BLUR, CB_FOCUS, CB_POINTERLOCKERROR,
  CB_GAMEPAD_CONNECTED, CB_GAMEPAD_DISCONNECTED,
  CB_COUNT
};

static u32 g_callbacks[CB_COUNT];
static u32 g_user_data[CB_COUNT];

#define WIN_REGISTER(name, kind)                                              \
  u32 w2c_env_##name(struct w2c_env* env, u32 target, u32 user, u32 capture,   \
                     u32 callback, u32 thread) {                              \
    (void)env; (void)target; (void)capture; (void)thread;                      \
    g_callbacks[kind] = callback;                                             \
    g_user_data[kind] = user;                                                 \
    return 0; /* EMSCRIPTEN_RESULT_SUCCESS */                                  \
  }

WIN_REGISTER(emscripten_set_keydown_callback_on_thread, CB_KEYDOWN)
WIN_REGISTER(emscripten_set_keyup_callback_on_thread, CB_KEYUP)
WIN_REGISTER(emscripten_set_mousedown_callback_on_thread, CB_MOUSEDOWN)
WIN_REGISTER(emscripten_set_mouseup_callback_on_thread, CB_MOUSEUP)
WIN_REGISTER(emscripten_set_mousemove_callback_on_thread, CB_MOUSEMOVE)
WIN_REGISTER(emscripten_set_mouseenter_callback_on_thread, CB_MOUSEENTER)
WIN_REGISTER(emscripten_set_mouseleave_callback_on_thread, CB_MOUSELEAVE)
WIN_REGISTER(emscripten_set_wheel_callback_on_thread, CB_WHEEL)
WIN_REGISTER(emscripten_set_touchstart_callback_on_thread, CB_TOUCHSTART)
WIN_REGISTER(emscripten_set_touchend_callback_on_thread, CB_TOUCHEND)
WIN_REGISTER(emscripten_set_touchmove_callback_on_thread, CB_TOUCHMOVE)
WIN_REGISTER(emscripten_set_touchcancel_callback_on_thread, CB_TOUCHCANCEL)
WIN_REGISTER(emscripten_set_blur_callback_on_thread, CB_BLUR)
WIN_REGISTER(emscripten_set_focus_callback_on_thread, CB_FOCUS)
WIN_REGISTER(emscripten_set_pointerlockerror_callback_on_thread, CB_POINTERLOCKERROR)

#define WIN_REGISTER_GAMEPAD(name, kind)                                      \
  u32 w2c_env_##name(struct w2c_env* env, u32 user, u32 capture,               \
                     u32 callback, u32 thread) {                              \
    (void)env; (void)capture; (void)thread;                                   \
    g_callbacks[kind] = callback;                                             \
    g_user_data[kind] = user;                                                 \
    return 0;                                                                 \
  }

WIN_REGISTER_GAMEPAD(emscripten_set_gamepadconnected_callback_on_thread, CB_GAMEPAD_CONNECTED)
WIN_REGISTER_GAMEPAD(emscripten_set_gamepaddisconnected_callback_on_thread, CB_GAMEPAD_DISCONNECTED)

static void window_log_once(const char* what) {
  static char seen[128][96];
  static unsigned n;
  for (unsigned i = 0; i < n; ++i)
    if (!strcmp(seen[i], what)) return;
  if (n < 128) {
    snprintf(seen[n], sizeof seen[n], "%s", what);
    ++n;
  }
  fprintf(stderr, "[window] %s\n", what);
}

/* Invoke a registered wasm callback.  The table comes from the engine's own
 * exported accessor, so no function indices are compiled into this layer. */
#if defined(WEBLOX_HAVE_SDL3)
static void dispatch(u32 kind, int event_type, u32 event_addr) {
  u32 callback = g_callbacks[kind];
  if (!callback || !event_addr || !weblox_instance) return;
  wasm_rt_funcref_table_t* table =
      w2c_WebClient_0x5F_indirect_function_table(weblox_instance);
  if (!table || callback >= table->size) return;
  wasm_rt_funcref_t fn = table->data[callback];
  if (!fn.func) return;
  typedef u32 (*event_cb)(void*, int, u32, u32);
  ((event_cb)fn.func)(fn.module_instance, event_type, event_addr,
                      g_user_data[kind]);
}
#endif /* WEBLOX_HAVE_SDL3 */

/* ------------------------------------------------------------------------- */
/* Guest scratch: event structs are allocated in linear memory per frame      */
/* ------------------------------------------------------------------------- */

#define WIN_SCRATCH 4096
static u32 g_scratch;
static u32 g_scratch_used;

static u32 scratch(u32 n) {
  if (!weblox_instance) return 0;
  if (!g_scratch) g_scratch = w2c_WebClient_malloc(weblox_instance, WIN_SCRATCH);
  if (!g_scratch) return 0;
  if (g_scratch_used + n > WIN_SCRATCH) return 0;
  u32 at = g_scratch + g_scratch_used;
  g_scratch_used += (n + 3u) & ~3u;
  return at;
}

static void put_at(u32 addr, size_t off, const void* src, size_t n) {
  unsigned char* p = guest_ptr(addr + (u32)off, n);
  if (p) memcpy(p, src, n);
}

#if defined(WEBLOX_HAVE_SDL3)
static void put_u32_at(u32 addr, size_t off, u32 v) { put_at(addr, off, &v, 4); }

static void put_i32_at(u32 addr, size_t off, int v) { put_at(addr, off, &v, 4); }
static void put_f64_at(u32 addr, size_t off, double v) { put_at(addr, off, &v, 8); }
static void put_bool_at(u32 addr, size_t off, bool v) { put_at(addr, off, &v, 1); }

static void put_str_at(u32 addr, size_t off, const char* s, size_t cap) {
  unsigned char* p = guest_ptr(addr + (u32)off, cap);
  if (!p) return;
  memset(p, 0, cap);
  if (s) {
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(p, s, n);
  }
}
#endif /* WEBLOX_HAVE_SDL3 */

/* emscripten/html5.h layouts (32-bit wasm: double 8, int 4, bool 1). */
typedef struct {
  double timestamp;
  unsigned location;
  bool ctrlKey, shiftKey, altKey, metaKey, repeat;
  unsigned charCode, keyCode, which;
  char key[32], code[32], charValue[32], locale[32];
} EmsKeyboard;

typedef struct {
  double timestamp;
  int screenX, screenY, clientX, clientY;
  bool ctrlKey, shiftKey, altKey, metaKey;
  unsigned short button, buttons;
  int movementX, movementY, targetX, targetY, canvasX, canvasY, padding;
} EmsMouse;

typedef struct {
  EmsMouse mouse;
  double deltaX, deltaY, deltaZ;
  unsigned deltaMode;
} EmsWheel;

typedef struct { char nodeName[128], id[128]; } EmsFocus;

typedef struct {
  int identifier, screenX, screenY, clientX, clientY, pageX, pageY;
  bool isChanged, onTarget;
  int targetX, targetY, canvasX, canvasY;
} EmsTouchPoint;

typedef struct {
  double timestamp;
  int numTouches;
  bool ctrlKey, shiftKey, altKey, metaKey;
  EmsTouchPoint touches[32];
} EmsTouch;

typedef struct {
  double timestamp;
  int numAxes, numButtons;
  double axis[64], analogButton[64];
  bool digitalButton[64];
  bool connected;
  int index;
  char id[64], mapping[64];
} EmsGamepad;

/* ------------------------------------------------------------------------- */
/* SDL event -> guest event structs                                           */
/* ------------------------------------------------------------------------- */

#if defined(WEBLOX_HAVE_SDL3)
static u32 dom_keycode(SDL_Keycode key) {
  if (key >= SDLK_A && key <= SDLK_Z) return (u32)('A' + (key - SDLK_A));
  if (key >= SDLK_0 && key <= SDLK_9) return (u32)('0' + (key - SDLK_0));
  if (key >= SDLK_F1 && key <= SDLK_F12) return 112u + (u32)(key - SDLK_F1);
  switch (key) {
    case SDLK_RETURN: case SDLK_RETURN2: return 13;
    case SDLK_ESCAPE: return 27;
    case SDLK_BACKSPACE: return 8;
    case SDLK_TAB: return 9;
    case SDLK_SPACE: return 32;
    case SDLK_DELETE: return 46;
    case SDLK_LEFT: return 37;
    case SDLK_UP: return 38;
    case SDLK_RIGHT: return 39;
    case SDLK_DOWN: return 40;
    default: return 0;
  }
}

static void fill_keyboard(u32 at, bool down, const SDL_KeyboardEvent* ke) {
  u32 kc = dom_keycode(ke->key);
  unsigned loc = 0;
  if (ke->key == SDLK_LSHIFT || ke->key == SDLK_LCTRL || ke->key == SDLK_LALT || ke->key == SDLK_LGUI)
    loc = 1;
  if (ke->key == SDLK_RSHIFT || ke->key == SDLK_RCTRL || ke->key == SDLK_RALT || ke->key == SDLK_RGUI)
    loc = 2;
  put_f64_at(at, offsetof(EmsKeyboard, timestamp), (double)ke->timestamp / 1e6);
  put_u32_at(at, offsetof(EmsKeyboard, location), loc);
  put_bool_at(at, offsetof(EmsKeyboard, ctrlKey), (ke->mod & SDL_KMOD_CTRL) != 0);
  put_bool_at(at, offsetof(EmsKeyboard, shiftKey), (ke->mod & SDL_KMOD_SHIFT) != 0);
  put_bool_at(at, offsetof(EmsKeyboard, altKey), (ke->mod & SDL_KMOD_ALT) != 0);
  put_bool_at(at, offsetof(EmsKeyboard, metaKey), (ke->mod & SDL_KMOD_GUI) != 0);
  put_bool_at(at, offsetof(EmsKeyboard, repeat), ke->repeat != 0);
  put_u32_at(at, offsetof(EmsKeyboard, charCode), down && kc >= 32 && kc < 127 ? kc : 0);
  put_u32_at(at, offsetof(EmsKeyboard, keyCode), kc);
  put_u32_at(at, offsetof(EmsKeyboard, which), kc);
  put_str_at(at, offsetof(EmsKeyboard, key), SDL_GetKeyName(ke->key), 32);
  put_str_at(at, offsetof(EmsKeyboard, code), SDL_GetScancodeName(ke->scancode), 32);
}

static u32 dom_button(Uint8 button) {
  if (button == 1) return 0; /* left */
  if (button == 2) return 1; /* middle */
  if (button == 3) return 2; /* right */
  return (u32)(button ? button - 1 : 0);
}

static u32 dom_buttons(void) {
  Uint32 mask = SDL_GetMouseState(NULL, NULL);
  u32 bits = 0;
  if (mask & SDL_BUTTON_LMASK) bits |= 1;
  if (mask & SDL_BUTTON_MMASK) bits |= 2;
  if (mask & SDL_BUTTON_RMASK) bits |= 4;
  return bits;
}

static void fill_mouse(u32 at, int x, int y, int dx, int dy, Uint16 button, Uint64 timestamp) {
  put_f64_at(at, offsetof(EmsMouse, timestamp), (double)timestamp / 1e6);
  put_i32_at(at, offsetof(EmsMouse, screenX), x);
  put_i32_at(at, offsetof(EmsMouse, screenY), y);
  put_i32_at(at, offsetof(EmsMouse, clientX), x);
  put_i32_at(at, offsetof(EmsMouse, clientY), y);
  put_bool_at(at, offsetof(EmsMouse, ctrlKey), (SDL_GetModState() & SDL_KMOD_CTRL) != 0);
  put_bool_at(at, offsetof(EmsMouse, shiftKey), (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
  put_bool_at(at, offsetof(EmsMouse, altKey), (SDL_GetModState() & SDL_KMOD_ALT) != 0);
  put_bool_at(at, offsetof(EmsMouse, metaKey), (SDL_GetModState() & SDL_KMOD_GUI) != 0);
  put_at(at, offsetof(EmsMouse, button), &button, 2);
  Uint16 buttons = (Uint16)dom_buttons();
  put_at(at, offsetof(EmsMouse, buttons), &buttons, 2);
  put_i32_at(at, offsetof(EmsMouse, movementX), dx);
  put_i32_at(at, offsetof(EmsMouse, movementY), dy);
  put_i32_at(at, offsetof(EmsMouse, targetX), x);
  put_i32_at(at, offsetof(EmsMouse, targetY), y);
}

static void fill_touch(u32 at, int type, const SDL_TouchFingerEvent* finger) {
  put_f64_at(at, offsetof(EmsTouch, timestamp), (double)finger->timestamp / 1e6);
  put_i32_at(at, offsetof(EmsTouch, numTouches), 1);
  size_t p = offsetof(EmsTouch, touches[0]);
  put_i32_at(at, p + offsetof(EmsTouchPoint, identifier), (int)finger->fingerID);
  int x = (int)(finger->x * g_width), y = (int)(finger->y * g_height);
  put_i32_at(at, p + offsetof(EmsTouchPoint, screenX), x);
  put_i32_at(at, p + offsetof(EmsTouchPoint, screenY), y);
  put_i32_at(at, p + offsetof(EmsTouchPoint, clientX), x);
  put_i32_at(at, p + offsetof(EmsTouchPoint, clientY), y);
  put_i32_at(at, p + offsetof(EmsTouchPoint, pageX), x);
  put_i32_at(at, p + offsetof(EmsTouchPoint, pageY), y);
  put_bool_at(at, p + offsetof(EmsTouchPoint, isChanged), type != SDL_EVENT_FINGER_MOTION);
  put_bool_at(at, p + offsetof(EmsTouchPoint, onTarget), true);
  put_i32_at(at, p + offsetof(EmsTouchPoint, targetX), x);
  put_i32_at(at, p + offsetof(EmsTouchPoint, targetY), y);
  (void)type;
}

static void fill_gamepad(u32 at, SDL_Gamepad* pad, bool connected, int index) {
  if (!pad) return;
  put_f64_at(at, offsetof(EmsGamepad, timestamp), (double)SDL_GetTicksNS() / 1e6);
  put_i32_at(at, offsetof(EmsGamepad, numAxes), 0);
  put_i32_at(at, offsetof(EmsGamepad, numButtons), 0);
  for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && i < 64; ++i)
    put_f64_at(at, offsetof(EmsGamepad, axis) + sizeof(double) * (size_t)i,
               SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)i) / 32767.0);
  put_i32_at(at, offsetof(EmsGamepad, numAxes), SDL_GAMEPAD_AXIS_COUNT);
  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && i < 64; ++i) {
    bool pressed = SDL_GetGamepadButton(pad, (SDL_GamepadButton)i);
    put_f64_at(at, offsetof(EmsGamepad, analogButton) + sizeof(double) * (size_t)i,
               pressed ? 1.0 : 0.0);
    put_bool_at(at, offsetof(EmsGamepad, digitalButton) + (size_t)i, pressed);
  }
  put_i32_at(at, offsetof(EmsGamepad, numButtons), SDL_GAMEPAD_BUTTON_COUNT);
  put_bool_at(at, offsetof(EmsGamepad, connected), connected);
  put_i32_at(at, offsetof(EmsGamepad, index), index);
  put_str_at(at, offsetof(EmsGamepad, id), SDL_GetGamepadName(pad), 64);
  put_str_at(at, offsetof(EmsGamepad, mapping), "standard", 64);
}

static void pump_sdl_events(void) {
  SDL_Event event;
  g_scratch_used = 0;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_EVENT_QUIT:
        g_should_close = 1;
        return;
      case SDL_EVENT_WINDOW_RESIZED:
        g_width = event.window.data1;
        g_height = event.window.data2;
        break;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP: {
        bool down = event.type == SDL_EVENT_KEY_DOWN;
        u32 at = scratch(sizeof(EmsKeyboard));
        if (!at) break;
        memset(guest_ptr(at, sizeof(EmsKeyboard)), 0, sizeof(EmsKeyboard));
        fill_keyboard(at, down, &event.key);
        dispatch(down ? CB_KEYDOWN : CB_KEYUP,
                 down ? 2 /* KEYDOWN */ : 3 /* KEYUP */, at);
        break;
      }
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        u32 at = scratch(sizeof(EmsMouse));
        if (!at) break;
        memset(guest_ptr(at, sizeof(EmsMouse)), 0, sizeof(EmsMouse));
        Uint16 button = (Uint16)dom_button(event.button.button);
        fill_mouse(at, (int)event.button.x, (int)event.button.y, 0, 0, button,
                   event.button.timestamp);
        bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        dispatch(down ? CB_MOUSEDOWN : CB_MOUSEUP,
                 down ? 5 /* MOUSEDOWN */ : 6 /* MOUSEUP */, at);
        break;
      }
      case SDL_EVENT_MOUSE_MOTION: {
        u32 at = scratch(sizeof(EmsMouse));
        if (!at) break;
        memset(guest_ptr(at, sizeof(EmsMouse)), 0, sizeof(EmsMouse));
        fill_mouse(at, (int)event.motion.x, (int)event.motion.y,
                   (int)event.motion.xrel, (int)event.motion.yrel, 0xffffu,
                   event.motion.timestamp);
        dispatch(CB_MOUSEMOVE, 8 /* MOUSEMOVE */, at);
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL: {
        u32 at = scratch(sizeof(EmsWheel));
        if (!at) break;
        memset(guest_ptr(at, sizeof(EmsWheel)), 0, sizeof(EmsWheel));
        fill_mouse(at, (int)event.wheel.mouse_x, (int)event.wheel.mouse_y, 0, 0,
                   0xffffu, event.wheel.timestamp);
        put_f64_at(at, offsetof(EmsWheel, deltaX), (double)event.wheel.x * 100.0);
        put_f64_at(at, offsetof(EmsWheel, deltaY), -(double)event.wheel.y * 100.0);
        put_f64_at(at, offsetof(EmsWheel, deltaZ), 0.0);
        put_u32_at(at, offsetof(EmsWheel, deltaMode), 0);
        dispatch(CB_WHEEL, 9 /* WHEEL */, at);
        break;
      }
      case SDL_EVENT_FINGER_DOWN:
      case SDL_EVENT_FINGER_UP:
      case SDL_EVENT_FINGER_MOTION: {
        int type = event.type == SDL_EVENT_FINGER_DOWN ? 22 :
                   event.type == SDL_EVENT_FINGER_UP ? 23 : 24;
        u32 kind = event.type == SDL_EVENT_FINGER_DOWN ? CB_TOUCHSTART :
                   event.type == SDL_EVENT_FINGER_UP ? CB_TOUCHEND : CB_TOUCHMOVE;
        u32 at = scratch(sizeof(EmsTouch));
        if (!at) break;
        memset(guest_ptr(at, sizeof(EmsTouch)), 0, sizeof(EmsTouch));
        fill_touch(at, (int)event.type, &event.tfinger);
        dispatch(kind, type, at);
        break;
      }
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
      case SDL_EVENT_WINDOW_FOCUS_LOST: {
        u32 at = scratch(sizeof(EmsFocus));
        if (!at) break;
        memset(guest_ptr(at, sizeof(EmsFocus)), 0, sizeof(EmsFocus));
        put_str_at(at, offsetof(EmsFocus, nodeName), "canvas", 128);
        put_str_at(at, offsetof(EmsFocus, id), "canvas", 128);
        bool gained = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        dispatch(gained ? CB_FOCUS : CB_BLUR, gained ? 13 : 12, at);
        break;
      }
      case SDL_EVENT_GAMEPAD_ADDED:
      case SDL_EVENT_GAMEPAD_REMOVED: {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(event.gdevice.which);
        u32 at = scratch(sizeof(EmsGamepad));
        if (at) {
          memset(guest_ptr(at, sizeof(EmsGamepad)), 0, sizeof(EmsGamepad));
          fill_gamepad(at, pad, event.type == SDL_EVENT_GAMEPAD_ADDED, 0);
          dispatch(event.type == SDL_EVENT_GAMEPAD_ADDED ? CB_GAMEPAD_CONNECTED
                                                        : CB_GAMEPAD_DISCONNECTED,
                   event.type == SDL_EVENT_GAMEPAD_ADDED ? 26 : 27, at);
        }
        if (pad) SDL_CloseGamepad(pad);
        break;
      }
      default:
        break;
    }
  }
}
#endif /* WEBLOX_HAVE_SDL3 */

/* ------------------------------------------------------------------------- */
/* Canvas / capability imports                                                */
/* ------------------------------------------------------------------------- */

u32 w2c_env_emscripten_get_canvas_element_size(struct w2c_env* env, u32 target,
                                                u32 width_ptr, u32 height_ptr) {
  (void)env; (void)target;
  guest_put_u32(width_ptr, (u32)g_width);
  guest_put_u32(height_ptr, (u32)g_height);
  return 0;
}

u32 w2c_env_emscripten_set_canvas_element_size(struct w2c_env* env, u32 target,
                                               u32 width, u32 height) {
  (void)env; (void)target;
  if (width > 1 && height > 1) {
    g_width = (int)width;
    g_height = (int)height;
#if defined(WEBLOX_HAVE_SDL3)
    if (g_window) SDL_SetWindowSize(g_window, g_width, g_height);
#endif
  }
  return 0;
}

u32 w2c_env_emscripten_get_element_css_size(struct w2c_env* env, u32 target,
                                            u32 width_ptr, u32 height_ptr) {
  (void)env; (void)target;
  double w = (double)g_width, h = (double)g_height;
  put_at(width_ptr, 0, &w, 8);
  put_at(height_ptr, 0, &h, 8);
  return 0;
}

f64 w2c_env_emscripten_get_device_pixel_ratio(struct w2c_env* env) {
  (void)env;
#if defined(WEBLOX_HAVE_SDL3)
  float scale = g_window ? SDL_GetWindowDisplayScale(g_window) : 1.0f;
  return (f64)scale;
#else
  return 1.0;
#endif
}

f64 w2c_env_get_effective_device_pixel_ratio(struct w2c_env* env) {
  return w2c_env_emscripten_get_device_pixel_ratio(env);
}

u32 w2c_env_has_keyboard(struct w2c_env* env) { (void)env; return 1; }
u32 w2c_env_has_mouse(struct w2c_env* env) { (void)env; return 1; }

u32 w2c_env_applyPendingCanvasResize(struct w2c_env* env, u32 a, u32 b) {
  (void)env; (void)a; (void)b;
  return 0;
}

u32 w2c_env_emscripten_request_pointerlock(struct w2c_env* env, u32 target, u32 defer) {
  (void)env; (void)target; (void)defer;
  window_log_once("pointer lock requested (not available natively)");
  return -5; /* EMSCRIPTEN_RESULT_FAILED_NOT_ALLOWED */
}

u32 w2c_env_emscripten_exit_pointerlock(struct w2c_env* env) {
  (void)env;
  return 0;
}

u32 w2c_env_emscripten_sample_gamepad_data(struct w2c_env* env) {
  (void)env;
  return 0;
}

u32 w2c_env_emscripten_get_num_gamepads(struct w2c_env* env) {
  (void)env;
#if defined(WEBLOX_HAVE_SDL3)
  int count = 0;
  SDL_JoystickID* pads = SDL_GetGamepads(&count);
  if (pads) SDL_free(pads);
  return (u32)(count > 0 ? count : 0);
#else
  return 0;
#endif
}

u32 w2c_env_emscripten_get_gamepad_status(struct w2c_env* env, u32 index, u32 out) {
  (void)env;
  u32 at = scratch(sizeof(EmsGamepad));
  if (!at) return -8; /* EMSCRIPTEN_RESULT_INVALID_PARAM */
  memset(guest_ptr(at, sizeof(EmsGamepad)), 0, sizeof(EmsGamepad));
#if defined(WEBLOX_HAVE_SDL3)
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (ids && index < (u32)count) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[index]);
    fill_gamepad(at, pad, true, (int)index);
    if (pad) SDL_CloseGamepad(pad);
  }
  if (ids) SDL_free(ids);
#endif
  unsigned char* dst = guest_ptr(out, sizeof(EmsGamepad));
  if (dst) memcpy(dst, guest_ptr(at, sizeof(EmsGamepad)), sizeof(EmsGamepad));
  return 0;
}

/* ------------------------------------------------------------------------- */
/* Public window API                                                          */
/* ------------------------------------------------------------------------- */

bool weblox_window_init(int width, int height) {
  if (width > 0) g_width = width;
  if (height > 0) g_height = height;
#if defined(WEBLOX_HAVE_SDL3)
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "[window] SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  g_window = SDL_CreateWindow("Weblox", g_width, g_height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!g_window) {
    fprintf(stderr, "[window] SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }
  g_gl_context = SDL_GL_CreateContext(g_window);
  if (!g_gl_context || !SDL_GL_MakeCurrent(g_window, g_gl_context)) {
    fprintf(stderr, "[window] GLES context failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_GL_SetSwapInterval(1);
  if (!gl_init(weblox_window_gl_loader())) {
    fprintf(stderr, "[window] GL loader failed\n");
    return false;
  }
  g_ready = 1;
  fprintf(stderr, "[window] SDL3 GLES window %dx%d ready\n", g_width, g_height);
  return true;
#else
  window_log_once("SDL3 not compiled in (headless mode)");
  return false;
#endif
}

bool weblox_window_pump(void) {
  wt_pump();
#if defined(WEBLOX_HAVE_SDL3)
  if (g_ready) pump_sdl_events();
#endif
  return !g_should_close;
}

void weblox_window_present(void) {
#if defined(WEBLOX_HAVE_SDL3)
  if (g_ready && g_window) SDL_GL_SwapWindow(g_window);
#else
  window_log_once("present (no window)");
#endif
}

void weblox_window_size(int* width, int* height) {
  if (width) *width = g_width;
  if (height) *height = g_height;
}

bool weblox_window_should_close(void) { return g_should_close != 0; }

void weblox_window_shutdown(void) {
#if defined(WEBLOX_HAVE_SDL3)
  gl_shutdown();
  webgpu_shutdown();
  wt_shutdown();
  if (g_gl_context) {
    SDL_GL_MakeCurrent(NULL, NULL);
    SDL_GL_DestroyContext(g_gl_context);
    g_gl_context = NULL;
  }
  if (g_window) {
    SDL_DestroyWindow(g_window);
    g_window = NULL;
  }
  if (g_scratch && weblox_instance) {
    w2c_WebClient_free(weblox_instance, g_scratch);
    g_scratch = 0;
  }
  SDL_Quit();
#else
  window_log_once("shutdown (no window)");
#endif
  memset(g_callbacks, 0, sizeof g_callbacks);
  g_ready = 0;
}

weblox_gl_proc_fn weblox_window_gl_loader(void) {
#if defined(WEBLOX_HAVE_SDL3)
  return (weblox_gl_proc_fn)SDL_GL_GetProcAddress;
#else
  window_log_once("GL loader unavailable (SDL3 not compiled in)");
  return NULL;
#endif
}
