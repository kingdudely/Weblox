// Stackful fibers for the engine's JSPI-style suspension.
//
// POSIX: ucontext (libucontext on platforms where it is deprecated).
// Windows: CreateFiber / SwitchToFiber.
// The default stub compiles everywhere so the skeleton builds without them.
#pragma once

#include <cstddef>
#include <functional>

namespace weblox::fiber {

using Entry = std::function<void()>;

bool init(std::size_t stack_size);
void* create(Entry entry);
void switch_to(void* fiber);
void* current();
void destroy(void* fiber);
void shutdown();

}  // namespace weblox::fiber
