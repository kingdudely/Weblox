#include "fiber.h"

#ifndef WEBLOX_USE_UCONTEXT

namespace weblox::fiber {

bool init(std::size_t) { return false; }
void* create(Entry) { return nullptr; }
void switch_to(void*) {}
void* current() { return nullptr; }
void destroy(void*) {}
void shutdown() {}

}  // namespace weblox::fiber

#endif  // !WEBLOX_USE_UCONTEXT
