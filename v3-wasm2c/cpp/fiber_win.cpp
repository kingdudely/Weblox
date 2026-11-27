// Windows stackful fibers.  Compile on Windows with -DWEBLOX_USE_WINDOWS_FIBERS
// instead of fiber_stub.cpp / fiber_ucontext.cpp.
#if defined(WEBLOX_USE_WINDOWS_FIBERS) && defined(_WIN32)

#include "fiber.h"

#include <windows.h>

#include <utility>
#include <vector>

namespace weblox::fiber {
namespace {

struct Fiber {
  LPVOID handle = nullptr;
  Entry entry;
};

thread_local Fiber* t_current = nullptr;
thread_local LPVOID t_main = nullptr;

void WINAPI trampoline(LPVOID param) {
  auto* fiber = static_cast<Fiber*>(param);
  t_current = fiber;
  if (fiber && fiber->entry) fiber->entry();
  if (t_main) SwitchToFiber(t_main);
}

}  // namespace

bool init(std::size_t) {
  if (!t_main) t_main = ConvertThreadToFiber(nullptr);
  return t_main != nullptr;
}

void* create(Entry entry) {
  auto* fiber = new Fiber();
  fiber->entry = std::move(entry);
  fiber->handle = CreateFiber(64 * 1024, trampoline, fiber);
  return fiber;
}

void switch_to(void* handle) {
  auto* fiber = static_cast<Fiber*>(handle);
  if (!fiber || !fiber->handle) return;
  Fiber* previous = t_current;
  t_current = fiber;
  SwitchToFiber(fiber->handle);
  t_current = previous;
}

void* current() { return t_current; }

void destroy(void* handle) {
  auto* fiber = static_cast<Fiber*>(handle);
  if (!fiber) return;
  if (fiber->handle) DeleteFiber(fiber->handle);
  delete fiber;
}

void shutdown() { t_current = nullptr; }

}  // namespace weblox::fiber

#endif  // Windows fibers
