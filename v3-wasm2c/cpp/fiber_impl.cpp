// JSPI-style suspended fibers for the portable C++ host.
//
// Ported from v3-wasm2c/fiber.c.  The guest protocol is unchanged:
//   * a fibre is created with a guest handle, a fresh guest stack window and
//     an entry index into the module's indirect function table;
//   * every suspension point files a slot under a guest-supplied key;
//   * a transfer restores the target slot's guest SP / stack bounds and
//     returns the resumer's key to it;
//   * a futex wait parks the current point and switches back to the creator,
//     exactly like a JSPI suspension, and the host pump resumes the slot.
//
// This file contains the platform code (POSIX ucontext / Win32 fibers) so no
// other host translation unit needs to know about ucontext_t or windows.h.
//
// Every wasm call made here is wrapped in wasm_rt_try + wasm_rt_set_unwind_target,
// as the main loop does.  The active unwind target and the wasm call depth are
// saved per slot and restored on every switch, so an exception thrown in one
// fibre never longjmps into a different fibre's stack.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#else
#include <ucontext.h>
#endif

extern "C" {
extern w2c_WebClient* g_wc_instance;
}

namespace {

constexpr int k_FIB_MAX = 1024;
constexpr int k_SLOT_MAX = 4096;
constexpr std::size_t k_FIB_STACK = 32u * 1024u * 1024u;
constexpr std::uint32_t k_ROOT_KEY = 0u;
constexpr std::uint32_t k_FIB_KEY_BASE = 0x80000000u;

struct Fib;
struct Slot;

struct Fib {
  std::uint32_t handle = 0;
  std::uint32_t entry = 0;
  std::uint32_t arg = 0;
  bool used = false;
  bool dead = false;
  void* stack = nullptr;       // host stack of the guest fibre
  Slot* home = nullptr;        // creator's suspension point (key == handle)
  Slot* root = nullptr;        // root slot this fibre was spawned under
#if defined(_WIN32)
  void* fiber = nullptr;       // CreateFiber handle for the guest stack
#endif
};

struct Slot {
  std::uint32_t key = 0;
  std::uint32_t sp = 0;
  std::uint32_t hi = 0;        // guest stack base (high address)
  std::uint32_t lo = 0;        // guest stack end (low address)
  std::uint32_t ret = 0;       // value delivered by the resume
  std::uint32_t depth = 0;     // wasm call depth owed to this point
  bool used = false;
  bool is_root = false;
  bool done = false;
  bool has_unwind = false;
  WASM_RT_UNWIND_TARGET unwind{};
  Fib* owner = nullptr;        // nullptr == host / main thread point
  Slot* root = nullptr;        // the root slot this point belongs to
#if defined(_WIN32)
  void* resume = nullptr;      // fiber handle to switch back to
#else
  ucontext_t ctx{};
#endif
};

thread_local Fib g_fibs[k_FIB_MAX];
thread_local Slot g_slots[k_SLOT_MAX];
thread_local Fib* g_cur = nullptr;       // nullptr == root (host) context
thread_local Slot* g_cur_root = nullptr;
thread_local std::uint32_t g_key_seq = 0;
thread_local std::uint32_t g_depth = 0;  // host-side mirror; never touches wasm-rt's TLS

#if defined(_WIN32)
thread_local void* t_main_fiber = nullptr;
#else
thread_local ucontext_t t_main_ctx{};
thread_local bool t_main_ready = false;
#endif

w2c_WebClient* inst() { return g_wc_instance; }

bool verbose() {
  static const bool enabled = std::getenv("WC_FIB") != nullptr;
  return enabled;
}

// Forward declaration: the platform setup below needs the trampoline, which
// is defined after fiber_body().
#if defined(_WIN32)
void WINAPI fiber_trampoline(LPVOID param);
#else
void fiber_trampoline();
#endif

bool platform_ready() {
#if defined(_WIN32)
  if (!t_main_fiber) t_main_fiber = ConvertThreadToFiber(nullptr);
  return t_main_fiber != nullptr;
#else
  if (!t_main_ready) {
    getcontext(&t_main_ctx);
    t_main_ready = true;
  }
  return true;
#endif
}

// Save the platform-level resume point of the current context into a slot.
void platform_capture(Slot& s) {
#if defined(_WIN32)
  s.resume = g_cur ? g_cur->fiber : t_main_fiber;
#else
  getcontext(&s.ctx);
#endif
}

// Build the initial context of a new guest fibre in its slot.
void platform_start(Slot& s, Fib& f) {
#if defined(_WIN32)
  f.fiber = CreateFiber(k_FIB_STACK, &fiber_trampoline, f);
  s.resume = f.fiber;
#else
  getcontext(&s.ctx);
  s.ctx.uc_stack.ss_sp = f.stack;
  s.ctx.uc_stack.ss_size = k_FIB_STACK;
  s.ctx.uc_link = nullptr;
  makecontext(&s.ctx, &fiber_trampoline, 0);
#endif
}

void platform_switch(Slot& from, Slot& to) {
#if defined(_WIN32)
  (void)from;
  if (to.resume) SwitchToFiber(to.resume);
#else
  swapcontext(&from.ctx, &to.ctx);
#endif
}

void platform_delete(Fib& f) {
#if defined(_WIN32)
  if (f.fiber) DeleteFiber(f.fiber);
  f.fiber = nullptr;
#else
  (void)f;
#endif
}

void capture_guest(Slot& s) {
  if (auto* i = inst()) {
    s.sp = w2c_WebClient_get_stack_ptr(i);
    s.hi = w2c_WebClient_get_stack_base(i);
    s.lo = w2c_WebClient_get_stack_end(i);
  }
}

void restore_guest(const Slot& s) {
  if (auto* i = inst()) {
    w2c_WebClient_set_stack_limits(i, s.hi, s.lo);
    w2c_WebClient_set_stack_ptr(i, s.sp);
  }
}

void save_unwind(Slot& s) {
  if (auto* u = wasm_rt_get_unwind_target()) {
    s.unwind = *u;
    s.has_unwind = true;
  } else {
    s.has_unwind = false;
  }
}

void restore_unwind(Slot& s) {
  if (s.has_unwind) wasm_rt_set_unwind_target(&s.unwind);
}

Slot* cur_root() { return g_cur ? g_cur->root : g_cur_root; }

Slot* slot_for(std::uint32_t key) {
  for (int i = 0; i < k_SLOT_MAX; ++i)
    if (g_slots[i].used && g_slots[i].key == key) return &g_slots[i];
  return nullptr;
}

Slot* slot_alloc() {
  for (int i = 0; i < k_SLOT_MAX; ++i)
    if (!g_slots[i].used) return &g_slots[i];
  for (int i = 0; i < k_SLOT_MAX; ++i)
    if (!g_slots[i].is_root && g_slots[i].owner && g_slots[i].owner->dead)
      return &g_slots[i];
  return nullptr;
}

Slot* slot_new(std::uint32_t key) {
  Slot* s = slot_for(key);
  if (!s) s = slot_alloc();
  if (!s) return nullptr;
  std::memset(s, 0, sizeof(*s));
  s->used = true;
  s->key = key;
  s->owner = g_cur;
  s->root = cur_root();
  s->depth = g_depth;
  capture_guest(*s);
  save_unwind(*s);
  platform_capture(*s);
  return s;
}

Fib* find_fib(std::uint32_t handle) {
  for (int i = 0; i < k_FIB_MAX; ++i)
    if (g_fibs[i].used && !g_fibs[i].dead && g_fibs[i].handle == handle)
      return &g_fibs[i];
  return nullptr;
}

// Switch away from 'mine' into 'target'.  The calling point's wasm state is
// saved in 'mine' (slot_new already did that, refresh it here), and the
// target's state is installed before the switch.  On return our own state is
// restored again, so the two guest stacks never share a stack pointer.
void enter_slot(Slot* target, Slot* mine, std::uint32_t ret) {
  if (!target || !mine) return;
  target->ret = ret;
  capture_guest(*mine);
  save_unwind(*mine);
  g_depth = target->depth;
  restore_unwind(*target);
  restore_guest(*target);
  Fib* saved_cur = g_cur;
  Slot* saved_root = g_cur_root;
  g_cur = target->owner;
  g_cur_root = target->root ? target->root : g_cur_root;
  platform_switch(*mine, *target);
  g_cur_root = saved_root;
  g_cur = saved_cur;
  g_depth = mine->depth;
  restore_unwind(*mine);
  restore_guest(*mine);
}

void fiber_body(Fib* self) {
  g_depth = 0;  // a fresh guest wasm stack
  if (verbose())
    std::fprintf(stderr, "[fib] entry handle=%u fn=%u arg=%u\n",
                 self->handle, self->entry, self->arg);
  bool threw = false;
  WASM_RT_UNWIND_TARGET* outer = wasm_rt_get_unwind_target();
  WASM_RT_UNWIND_TARGET target;
  if (!wasm_rt_try(target)) {
    wasm_rt_set_unwind_target(&target);
    w2c_WebClient_fiber_entry(inst(), self->handle, self->entry, self->arg);
    wasm_rt_set_unwind_target(outer);
  } else {
    wasm_rt_set_unwind_target(outer);
    threw = true;
    std::fprintf(stderr, "[fib] entry threw handle=%u tag=%p bytes=%u\n",
                 self->handle, wasm_rt_exception_tag(), wasm_rt_exception_size());
  }
  self->dead = true;

  // Contexts of this fibre die with its host stack.  Keep the home slot: it
  // is the return path to the creator and is switched to just below.
  for (int i = 0; i < k_SLOT_MAX; ++i)
    if (g_slots[i].used && g_slots[i].owner == self && &g_slots[i] != self->home)
      g_slots[i].used = false;

  Slot* mine = slot_alloc();
  if (mine) {
    std::memset(mine, 0, sizeof(*mine));
    mine->used = true;
    mine->key = k_FIB_KEY_BASE | (++g_key_seq);
    mine->owner = self;
    mine->root = self->root;
    mine->depth = g_depth;
    capture_guest(*mine);
    save_unwind(*mine);
    platform_capture(*mine);
  }
  if (self->home) enter_slot(self->home, mine, threw ? 1u : 0u);
  std::fprintf(stderr, "[fib] trampoline fell through\n");
  std::abort();
}

#if defined(_WIN32)
void WINAPI fiber_trampoline(LPVOID param) {
  fiber_body(static_cast<Fib*>(param));
}
#else
void fiber_trampoline() { fiber_body(g_cur); }
#endif

}  // namespace

extern "C" {

u32 w2c_env_0x5F_asyncjs_0x5Fcreate_suspended_fiber_js(struct w2c_env*, u32 h,
                                                        u32 sp, u32 limit,
                                                        u32 fn, u32 arg) {
  (void)h;
  if (!inst() || !platform_ready()) return 0;
  Fib* f = nullptr;
  for (int i = 0; i < k_FIB_MAX; ++i)
    if (!g_fibs[i].used) { f = &g_fibs[i]; break; }
  if (!f) { std::fprintf(stderr, "[fib] create: fibre table full\n"); return 0; }
  std::memset(f, 0, sizeof(*f));
  f->used = true;
  f->handle = h;
  f->entry = fn;
  f->arg = arg;
  f->stack = std::malloc(k_FIB_STACK);
  if (!f->stack) { f->used = false; return 0; }

  // The home slot resumes the creator; file it before switching windows.
  Slot* home = slot_new(h);
  if (!home) { std::free(f->stack); f->used = false; return 0; }
  home->is_root = true;
  home->root = home;
  f->home = home;
  f->root = home;
  Fib* prev_cur = g_cur;
  Slot* prev_root = g_cur_root;
  g_cur_root = home;

  // The guest window handed to us: base == sp, end == limit, SP == sp.
  Slot* entry = slot_alloc();
  if (!entry) { std::free(f->stack); f->used = false; home->used = false; return 0; }
  std::memset(entry, 0, sizeof(*entry));
  entry->used = true;
  entry->key = sp ? sp - 1u : 0u;
  entry->owner = f;
  entry->root = home;
  entry->sp = sp;
  entry->hi = sp;
  entry->lo = limit;
  entry->depth = 0;
  entry->has_unwind = false;
  platform_start(*entry, *f);
  if (!f->stack) { f->used = false; home->used = false; return 0; }
#if defined(_WIN32)
  if (!f->fiber) { std::free(f->stack); f->used = false; home->used = false; return 0; }
#endif

  if (verbose())
    std::fprintf(stderr, "[fib] create handle=%u sp=%x limit=%x fn=%u\n",
                 h, sp, limit, fn);
  g_cur = f;
  enter_slot(entry, home, 0);
  g_cur = prev_cur;
  g_cur_root = prev_root;
  const u32 result = home->ret;
  if (f->dead) {
    platform_delete(*f);
    std::free(f->stack);
    f->stack = nullptr;
    f->used = false;
  }
  if (verbose())
    std::fprintf(stderr, "[fib] create returned handle=%u ret=%u\n", h, result);
  return result;
}

u32 w2c_env_0x5F_asyncjs_0x5Ffiber_transfer_js(struct w2c_env*, u32 from, u32 to) {
  if (from == to) return from;
  Slot* mine = slot_new(from);
  if (!mine) return from;
  std::uint32_t key = to;
  if (!key) {
    Slot* home = g_cur ? g_cur->home : nullptr;
    if (!home) { mine->used = false; return from; }
    key = home->key;
  }
  Slot* target = slot_for(key);
  if (!target) {
    target = slot_new(key);
    if (!target) { mine->used = false; return from; }
  }
  if (verbose())
    std::fprintf(stderr, "[fib] transfer %x -> %x\n", from, key);
  enter_slot(target, mine, from);
  const u32 result = mine->ret;
  mine->used = false;
  return result;
}

void w2c_env_fiber_destroy_js(struct w2c_env*, u32 h) {
  Fib* f = find_fib(h);
  if (!f) return;
  std::fprintf(stderr, "[fib] destroy handle=%u\n", h);
  f->dead = true;
  for (int i = 0; i < k_SLOT_MAX; ++i)
    if (g_slots[i].used && g_slots[i].owner == f && &g_slots[i] != f->home)
      g_slots[i].used = false;
  if (g_cur == f && f->home) {
    Slot* mine = slot_new(k_FIB_KEY_BASE | (++g_key_seq));
    enter_slot(f->home, mine, 0);
    g_cur = f->home->owner;  // enter_slot restored the (dead) fibre; undo that
    return;
  }
  platform_delete(*f);
  std::free(f->stack);
  f->stack = nullptr;
  f->used = false;
}

void w2c_env_emscripten_unwind_to_js_event_loop(struct w2c_env*) {
  // In the browser this unwinds the JSPI stack back to the event loop.  The
  // C++ host's guarded wasm calls already own the unwind target, so the
  // exception raised by emscripten_exit_with_live_runtime is caught there.
  // If it happens on a guest fibre, hand control back to its creator so the
  // suspended stack cannot leak into the next turn.
  std::fprintf(stderr, "[fib] unwind_to_js_event_loop\n");
  if (g_cur && g_cur->home) {
    Slot* home = g_cur->home;
    Slot* mine = slot_new(k_FIB_KEY_BASE | (++g_key_seq));
    enter_slot(home, mine, 0);
    g_cur = home->owner;
  }
}

// ---------------------------------------------------------------------------
// Host-side API used by the futex and deferred pump (declared locally in each
// consumer: this host intentionally has no new public headers).
// ---------------------------------------------------------------------------

int weblox_fiber_current_is_root(void) { return g_cur == nullptr ? 1 : 0; }

void* weblox_fiber_park_begin(void) {
  if (!g_cur || !g_cur->home) return nullptr;
  return slot_new(k_FIB_KEY_BASE | (++g_key_seq));
}

void weblox_fiber_park(void* token) {
  auto* s = static_cast<Slot*>(token);
  if (!s || !g_cur || !g_cur->home) return;
  enter_slot(g_cur->home, s, 0);
}

void weblox_fiber_resume_slot(void* token, u32 code) {
  auto* s = static_cast<Slot*>(token);
  if (!s || !s->used) return;
  if (s->owner && s->owner->dead) { s->used = false; return; }
  Slot tmp;
  std::memset(&tmp, 0, sizeof(tmp));
  tmp.used = true;
  tmp.key = k_FIB_KEY_BASE | (++g_key_seq);
  tmp.owner = g_cur;
  tmp.root = cur_root();
  tmp.depth = g_depth;
  capture_guest(tmp);
  save_unwind(tmp);
  platform_capture(tmp);
  enter_slot(s, &tmp, code);
}

void weblox_fiber_slot_free(void* token) {
  auto* s = static_cast<Slot*>(token);
  if (s) s->used = false;
}

u32 weblox_fiber_slot_ret(void* token) {
  auto* s = static_cast<Slot*>(token);
  return s ? s->ret : 0u;
}

int weblox_fiber_slot_is_root(void* token) {
  auto* s = static_cast<Slot*>(token);
  return s && s->is_root ? 1 : 0;
}

}  // extern "C"
