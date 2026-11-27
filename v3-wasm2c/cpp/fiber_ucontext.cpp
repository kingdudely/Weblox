// POSIX stackful fibers.  Compile with -DWEBLOX_USE_UCONTEXT instead of
// fiber_stub.cpp.  Windows uses CreateFiber in a separate file later.
#ifdef WEBLOX_USE_UCONTEXT

#include "fiber.h"

#include <ucontext.h>

#include <cstdlib>
#include <utility>
#include <vector>

namespace weblox::fiber {
namespace {

struct Fiber {
  ucontext_t context{};
  Entry entry;
  std::vector<char> stack;
};

thread_local Fiber* t_current = nullptr;
thread_local ucontext_t t_main{};
std::size_t g_stack_size = 0;

void trampoline() {
  Fiber* self = t_current;
  if (self && self->entry) self->entry();
  // Returning from a fiber goes back to whoever switched to it.
  if (t_current) swapcontext(&t_current->context, &t_main);
}

}  // namespace

bool init(std::size_t stack_size) {
  g_stack_size = stack_size ? stack_size : 64 * 1024;
  return true;
}

void* create(Entry entry) {
  auto* fiber = new Fiber();
  fiber->entry = std::move(entry);
  fiber->stack.resize(g_stack_size);
  getcontext(&fiber->context);
  fiber->context.uc_stack.ss_sp = fiber->stack.data();
  fiber->context.uc_stack.ss_size = fiber->stack.size();
  fiber->context.uc_link = &t_main;
  makecontext(&fiber->context, trampoline, 0);
  return fiber;
}

void switch_to(void* handle) {
  auto* fiber = static_cast<Fiber*>(handle);
  if (!fiber) return;
  Fiber* previous = t_current;
  t_current = fiber;
  swapcontext(previous ? &previous->context : &t_main, &fiber->context);
}

void* current() { return t_current; }

void destroy(void* handle) { delete static_cast<Fiber*>(handle); }

void shutdown() { t_current = nullptr; }

}  // namespace weblox::fiber

#endif  // WEBLOX_USE_UCONTEXT
