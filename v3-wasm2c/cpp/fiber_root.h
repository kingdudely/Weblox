// Host-side root fibers: JSPI-style suspension for main-thread wasm calls.
//
// The engine's wasm calls must not run on the raw host stack.  A root fiber
// has its own machine stack (ucontext on POSIX, CreateFiber on Windows) so an
// import that has to wait - notably rbxFutexWaitMainJspi - can capture its
// stack/unwind state, switch back to the host stack, and let the event loop
// keep turning.  A later weblox_futex_pump() resumes the parked slot.
//
// Usage from the main loop (inst is the w2c_WebClient instance):
//
//   weblox::fibers::enable_root_mode();          // once, before any root
//   auto main_root = weblox::fibers::run_on_root(
//       inst, [&] { w2c_WebClient_0x5F_main_argc_argv(inst, argc, argv); });
//   while (!weblox::fibers::root_done(main_root) && time_left) {
//     weblox::deferred::pump(inst);              // includes weblox_futex_pump()
//   }
//   weblox::fibers::root_free(main_root);
//
// A root handle stays valid until root_free().  root_done() becomes true
// when the body returned or threw; a wasm exception is caught on the fiber
// that threw it and reported to the host, never longjmp'd across stacks.
// Do not root_free() a still-parked root: a pending futex wakeup could resume
// a stack that no longer exists.
#pragma once

#include <functional>

extern "C" {
struct w2c_WebClient;
}

namespace weblox::fibers {

// Opaque handle for one invocation on a root fiber.
using RootHandle = void*;

// Enable root-fiber mode on this thread.  run_on_root() calls this itself,
// so it is optional; it is exposed for symmetry with the C reference host.
void enable_root_mode();

// True while executing on a root fiber (not the host stack, not an
// engine-created guest fiber).  Deferred callbacks use this to avoid
// spawning a nested root when they are already on one.
bool on_root();

// Run `fn` on a fresh root fiber.  Returns an opaque handle or nullptr if a
// fiber/stack could not be created.  `inst` supplies the guest stack state
// that is saved and restored at every suspension; nullptr uses the instance
// registered by host.cpp.
RootHandle run_on_root(w2c_WebClient* inst, std::function<void()> fn);

// True when the root's body has finished (returned or threw).
bool root_done(RootHandle handle);

// True when the root is in flight but currently parked in a futex wait.
bool root_suspended(RootHandle handle);

// True when the body ended via a wasm exception.  The exception was caught
// and reported on this fibre (stderr) and never crossed a stack boundary.
bool root_threw(RootHandle handle);

// Release a finished root and its host stack.  See the warning above.
void root_free(RootHandle handle);

}  // namespace weblox::fibers
