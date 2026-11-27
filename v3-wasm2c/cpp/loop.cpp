#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"
#include "loop.h"
#include "platform.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {
std::atomic<bool> g_scheduled{false};

// Browser glue wraps every task in try/catch: a wasm exception escaping a
// turn is logged and the loop continues.  Without an unwind target,
// wasm_rt_throw longjmps through a null pointer.
template <class F>
bool guarded(F&& fn) {
  WASM_RT_UNWIND_TARGET* outer = wasm_rt_get_unwind_target();
  WASM_RT_UNWIND_TARGET target;
  if (!wasm_rt_try(target)) {
    wasm_rt_set_unwind_target(&target);
    fn();
    wasm_rt_set_unwind_target(outer);
    return true;
  }
  wasm_rt_set_unwind_target(outer);
  std::fprintf(stderr, "[iter] wasm exception tag=%p bytes=%u\n",
               wasm_rt_exception_tag(), wasm_rt_exception_size());
  return false;
}
}  // namespace

extern "C" void w2c_env_schedule_main_loop(struct w2c_env*) {
  g_scheduled.store(true, std::memory_order_release);
}

namespace weblox::deferred {
int pump(w2c_WebClient* inst);
}

namespace weblox::loop {

bool scheduled() { return g_scheduled.load(std::memory_order_acquire); }

int run(w2c_WebClient* inst, std::chrono::milliseconds budget) {
  g_scheduled.store(false, std::memory_order_release);

  // Emscripten leaves holes in the indirect function table for lazy
  // initialisation; give them the first populated funcref before ctors.
  auto& table = inst->w2c_0x5F_indirect_function_table;
  if (table.size > 1 && table.data[1].func) {
    const auto fill = table.data[1];
    for (u64 i = 0; i < table.size; ++i)
      if (!table.data[i].func) table.data[i] = fill;
  }
  w2c_WebClient_0x5F_wasm_call_ctors(inst);

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
    guarded([&] { w2c_WebClient_0x5F_main_argc_argv(inst, 1, array); });
  } else {
    guarded([&] { w2c_WebClient_0x5F_main_argc_argv(inst, 0, 0); });
  }

  const auto deadline = std::chrono::steady_clock::now() + budget;
  int turns = 0;
  // Browsers call rAF every frame whether or not the engine set the
  // schedule_main_loop flag; join work is driven from those turns.
  while (std::chrono::steady_clock::now() < deadline) {
    weblox::deferred::pump(inst);
    guarded([&] { w2c_WebClient_main_loop_iteration(inst); });
    ++turns;
    platform::sleep_ms(1);
  }
  return turns;
}

}  // namespace weblox::loop
