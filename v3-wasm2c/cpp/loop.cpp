#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "loop.h"
#include "platform.h"

#include <atomic>
#include <chrono>

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
  // The engine's browser entry expects no host argv (it reads its config from
  // imports), so both arguments are zero.
  w2c_WebClient_0x5F_main_argc_argv(inst, 0, 0);

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
