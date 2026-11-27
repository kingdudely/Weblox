#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"
#include "loop.h"
#include "platform.h"

#include <atomic>
#include <chrono>
#include <cstring>

namespace {
std::atomic<bool> g_scheduled{false};
}  // namespace

extern "C" void w2c_env_schedule_main_loop(struct w2c_env*) {
  g_scheduled.store(true, std::memory_order_release);
}

namespace weblox::loop {

bool scheduled() { return g_scheduled.load(std::memory_order_acquire); }

int run(w2c_WebClient* inst, std::chrono::milliseconds budget) {
  g_scheduled.store(false, std::memory_order_release);

  // The generated export is `__main_argc_argv`, declared by name in WC.h.
  // Its arguments are a guest argc and a guest address of a pointer array, so
  // both are allocated in the module's own linear memory.
  constexpr char kArgv0[] = "/content/.keep";
  const u32 text = w2c_WebClient_malloc(inst, sizeof(kArgv0));
  const u32 array = w2c_WebClient_malloc(inst, 8);
  if (text && array) {
    if (auto* p = weblox::guest::ptr(text, sizeof(kArgv0)))
      std::memcpy(p, kArgv0, sizeof(kArgv0));
    weblox::guest::put_u32(array, text);
    weblox::guest::put_u32(array + 4, 0);
    w2c_WebClient_0x5F_main_argc_argv(inst, 1, array);
  } else {
    w2c_WebClient_0x5F_main_argc_argv(inst, 0, 0);
  }

  const auto deadline = std::chrono::steady_clock::now() + budget;
  int turns = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!scheduled()) {
      platform::sleep_ms(1);
      continue;
    }
    w2c_WebClient_main_loop_iteration(inst);
    ++turns;
  }
  return turns;
}

}  // namespace weblox::loop
