#include "platform.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace weblox::platform {
namespace {
std::mutex g_wait_mutex;
std::unordered_map<const volatile void*, std::condition_variable*> g_waiters;
}

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void sleep_ms(std::uint64_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void spawn_thread(std::function<void()> fn) {
  std::thread(std::move(fn)).detach();
}

void wait_on(const volatile void* address, std::uint32_t value) {
  std::unique_lock<std::mutex> lock(g_wait_mutex);
  auto& cv = g_waiters[address];
  if (!cv) cv = new std::condition_variable();
  (void)value;  // the guest re-checks the futex word itself
  cv->wait_for(lock, std::chrono::milliseconds(2));
}

void wake_on(const volatile void* address) {
  std::lock_guard<std::mutex> lock(g_wait_mutex);
  auto it = g_waiters.find(address);
  if (it != g_waiters.end() && it->second) it->second->notify_all();
}

}  // namespace weblox::platform
