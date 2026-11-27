// Portable platform services for the host.  Uses only the C++ standard
// library, so it builds on Linux, macOS and Windows without conditional code.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace weblox::platform {

std::uint64_t now_ms();
void sleep_ms(std::uint64_t ms);

// Spawn a detached thread; useful for the engine's pthread imports.
void spawn_thread(std::function<void()> fn);

// Cross-platform wait/wake on an address-sized word.  Falls back to a
// condition variable when the OS primitive is unavailable.
void wait_on(const volatile void* address, std::uint32_t value);
void wake_on(const volatile void* address);

}  // namespace weblox::platform
