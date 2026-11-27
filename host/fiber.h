// Native stack switching for JSPI-style imports.
//
// The browser's `__asyncjs__rbxFutexWaitMainJspi` suspends the wasm call and
// gives control back to the page event loop.  This header exposes the minimal
// ucontext-based fibre used by slot 5 (host/imports.cpp) and by the bounded
// turn driver (host/fiber.cpp installs the wrapped weblox::run_loop).
//
// All state is thread-local: a fibre created on one thread can only be
// yielded/resumed on that same thread.  See host/FIBER.md.
#pragma once

#include <cstdint>
#include <functional>

namespace weblox {
namespace fiber {

// True while a fibre-driven turn is installed on this thread.  Slot 5 uses
// this to choose the suspending path instead of the boot-time bounded poll.
bool enabled();

// True when the installed turn has yielded and is waiting for resume.
bool suspended();

// Called from inside the guest import (on the fibre stack).  Switches back
// to the driver and only returns after fiber_resume().  A call made from any
// other stack (e.g. a re-entrant RNA callback run by the driver) is a no-op.
void yield();

// True during the execution that immediately follows a yield() return.
// Slot 5 uses it to avoid suspending the same invocation twice.
bool resuming();

// Driver: state consumed by slot 5 after yield() returns.
void set_woken(bool woken);
bool woken();

// Driver: start a fresh fibre and run `turn` on its own stack.  Returns as
// soon as the guest returns from `turn` OR the guest yields.
void start(const std::function<int()>& turn);

// Driver: resume a suspended turn.  Returns when the guest yields again or
// the turn completes (see turn_completed() / turn_result()).
void resume(bool woken);

// Driver: result of the most recently completed turn.
bool turn_completed();
int turn_result();

// Driver: destroy the fibre stack.  Safe to call with a suspended turn; the
// abandoned stack is simply released.
void shutdown();

}  // namespace fiber
}  // namespace weblox
