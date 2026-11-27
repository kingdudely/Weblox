// Minimal Wasmtime 50 exception propagation for the C++ host.
//
// Evidence for the format and the resumption convention:
//   wasmtime-internal-unwinder-50.0.2/src/exception_table.rs  (serialization)
//   wasmtime-internal-unwinder-50.0.2/src/stackwalk.rs        (FP chain walk)
//   wasmtime-internal-unwinder-50.0.2/src/throw.rs            (handler search)
//   wasmtime-internal-unwinder-50.0.2/src/arch/x86.rs         (resume registers)
//   wasmtime-50.0.2/src/runtime/vm/throw.rs                   (handler lookup)
//   cranelift-codegen-0.137.2/src/isa/x64/abi.rs              (rax/rdx payload)
//
// No Cranelift codegen changes are needed: the try_call landing pads are
// already present in out.full.o.  The host only has to find one and enter it
// with the register/stack state Wasmtime's `Handler::resume` would use.
#define _GNU_SOURCE 1
#include "unwind.h"
#include "vmctx.h"
#include "pthread_start.h"

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace host {
namespace {

constexpr uint32_t kNoTag = 0xffffffffu;
constexpr unsigned kMaxFrames = 96;

// ---- bounded trace ---------------------------------------------------------
bool trace_enabled() {
  static const bool enabled = [] {
    const char* s = std::getenv("HOST_TRACE");
    return s && *s && std::strcmp(s, "0") != 0;
  }();
  return enabled;
}

void trace(const char* fmt, ...) {
  static unsigned n = 0;
  if (!trace_enabled() || n >= 96) return;
  ++n;
  std::fputs("[unwind] ", stderr);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

long current_tid() { return static_cast<long>(::syscall(SYS_gettid)); }

// A VMStoreContext copied into a worker vmctx inherits the main thread's exit
// PC/FP.  Those values point into a stack owned by another pthread: walking
// them faults.  Cache this thread's native stack range and refuse to
// dereference a frame pointer that belongs elsewhere.
struct StackRange {
  uintptr_t lo = 0, hi = 0;
  bool ready = false;
};
thread_local StackRange tl_stack;

bool in_current_stack(uintptr_t p) {
  if (!tl_stack.ready) {
    pthread_attr_t attr;
    if (::pthread_getattr_np(::pthread_self(), &attr) != 0) return false;
    void* base = nullptr;
    size_t size = 0;
    const bool ok = ::pthread_attr_getstack(&attr, &base, &size) == 0 && size > 16;
    ::pthread_attr_destroy(&attr);
    if (!ok) return false;
    tl_stack.lo = reinterpret_cast<uintptr_t>(base);
    tl_stack.hi = tl_stack.lo + size;
    tl_stack.ready = true;
  }
  return p > tl_stack.lo && p + 16 < tl_stack.hi;
}

// ---- exception table -------------------------------------------------------
struct SerializedTable {
  uintptr_t addr = 0;
  size_t size = 0;
  uint32_t callsite_count = 0;
  uint32_t handler_count = 0;
  const uint32_t* callsites = nullptr;
  const uint32_t* frame_offsets = nullptr;
  const uint32_t* ranges = nullptr;
  const uint32_t* tags = nullptr;
  const uint32_t* contexts = nullptr;
  const uint32_t* handlers = nullptr;
  bool ready = false;
};

SerializedTable g_table;
uintptr_t g_text_end = 0;

uint32_t rd32(const unsigned char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

uint64_t rd64(const unsigned char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

// The executable is a PIE: section addresses in the ELF are relative to the
// load bias, not absolute runtime addresses.
uintptr_t g_load_bias = 0;
int load_bias_cb(struct dl_phdr_info* info, size_t, void*) {
  if (info->dlpi_name && info->dlpi_name[0] == '\0') {
    g_load_bias = static_cast<uintptr_t>(info->dlpi_addr);
    return 1;  // stop at the main executable
  }
  return 0;
}

bool init_table() {
  ::dl_iterate_phdr(&load_bias_cb, nullptr);
  int fd = ::open("/proc/self/exe", O_RDONLY);
  if (fd < 0) {
    trace("open /proc/self/exe failed\n");
    return false;
  }
  unsigned char eh[64];
  bool ok = ::pread(fd, eh, sizeof(eh), 0) == ssize_t(sizeof(eh));
  if (!ok) trace("ELF header pread failed\n");
  if (ok && !(eh[0] == 0x7f && eh[1] == 'E' && eh[2] == 'L' && eh[3] == 'F' &&
              eh[4] == 2 && eh[5] == 1)) {
    trace("ELF header not ELF64 LE\n");
    ok = false;
  }
  if (ok) {
    const uint64_t shoff = rd64(eh + 0x28);
    const uint16_t shentsize = static_cast<uint16_t>(eh[0x3a] | (eh[0x3b] << 8));
    const uint16_t shnum = static_cast<uint16_t>(eh[0x3c] | (eh[0x3d] << 8));
    const uint16_t shstrndx = static_cast<uint16_t>(eh[0x3e] | (eh[0x3f] << 8));
    if (shentsize >= 64 && shnum > shstrndx && shnum < 4096) {
      std::vector<unsigned char> shdrs(static_cast<size_t>(shnum) * shentsize);
      ok = ::pread(fd, shdrs.data(), shdrs.size(), static_cast<off_t>(shoff)) ==
           static_cast<ssize_t>(shdrs.size());
      if (!ok) trace("section header pread failed\n");
      if (ok) {
        auto sh = [&](uint16_t i) { return shdrs.data() + size_t(i) * shentsize; };
        const unsigned char* strsec = sh(shstrndx);
        const uint64_t stroff = rd64(strsec + 0x18);
        const uint64_t strsize = rd64(strsec + 0x20);
        std::vector<char> names(static_cast<size_t>(strsize) + 1);
        ok = ::pread(fd, names.data(), strsize, static_cast<off_t>(stroff)) ==
             static_cast<ssize_t>(strsize);
        if (!ok) trace("shstrtab pread failed (off=%#llx size=%llu)\n",
                       static_cast<unsigned long long>(stroff),
                       static_cast<unsigned long long>(strsize));
        if (ok) {
          names.back() = 0;
          for (uint16_t i = 0; i < shnum; ++i) {
            const unsigned char* s = sh(i);
            const uint32_t nameoff = rd32(s);
            if (nameoff >= strsize) continue;
            if (std::strcmp(names.data() + nameoff, ".text") == 0) {
              g_text_end = static_cast<uintptr_t>(rd64(s + 0x10) + rd64(s + 0x20)) + g_load_bias;
            }
            if (std::strcmp(names.data() + nameoff, ".wasmtime.exceptions") != 0) continue;
            const uint64_t addr = rd64(s + 0x10);
            const uint64_t size = rd64(s + 0x20);
            if (addr == 0 || size < 8 || size > 128u * 1024 * 1024) {
              trace("bad section bounds\n");
              break;
            }
            const unsigned char* data =
                reinterpret_cast<const unsigned char*>(addr + g_load_bias);
            const uint32_t cs = rd32(data);
            const uint32_t hs = rd32(data + 4);
            const size_t expected = 8 + size_t(cs) * 12 + size_t(hs) * 12;
            if (expected != size || cs == 0 || hs == 0) {
              trace("bad table prefix cs=%u hs=%u expected=%zu\n", cs, hs, expected);
              break;
            }
            g_table.addr = static_cast<uintptr_t>(addr + g_load_bias);
            g_table.size = static_cast<size_t>(size);
            g_table.callsite_count = cs;
            g_table.handler_count = hs;
            g_table.callsites = reinterpret_cast<const uint32_t*>(data + 8);
            g_table.frame_offsets = g_table.callsites + cs;
            g_table.ranges = g_table.frame_offsets + cs;
            g_table.tags = g_table.ranges + cs;
            g_table.contexts = g_table.tags + hs;
            g_table.handlers = g_table.contexts + hs;
            g_table.ready = true;
            trace("table: %u callsites, %u handlers at %#lx (%zu bytes)\n", cs, hs,
                 static_cast<unsigned long>(addr), static_cast<size_t>(size));
            break;
          }
          if (!g_table.ready) trace(".wasmtime.exceptions section not found\n");
        }
      }
    }
  }
  ::close(fd);
  return g_table.ready;
}

struct HandlerRange {
  uint32_t frame_offset;
  uint32_t begin;
  uint32_t end;
  uint32_t index;
};

bool lookup_pc(uint32_t rel_pc, HandlerRange& out) {
  if (!g_table.ready) return false;
  const uint32_t* found = std::lower_bound(g_table.callsites,
                                           g_table.callsites + g_table.callsite_count, rel_pc);
  if (found == g_table.callsites + g_table.callsite_count || *found != rel_pc) return false;
  const uint32_t idx = static_cast<uint32_t>(found - g_table.callsites);
  out.frame_offset = g_table.frame_offsets[idx];
  out.end = g_table.ranges[idx];
  out.begin = idx ? g_table.ranges[idx - 1] : 0;
  out.index = idx;
  return out.end > out.begin && out.end <= g_table.handler_count;
}

// ---- resumption ------------------------------------------------------------
[[noreturn]] void resume_to_handler(const UnwindHandler& h, uint32_t exnref) {
  // Same register contract as wasmtime_unwinder::arch::x86::
  // resume_to_exception_handler: rax/rdx are the two Cranelift exception
  // payloads, rsp/rbp are restored and execution jumps to the landing pad.
  __asm__ volatile("mov %0, %%rsp\n\t"
                   "mov %1, %%rbp\n\t"
                   "jmp *%2\n\t"
                   :
                   : "c"(h.sp), "S"(h.fp), "D"(h.pc), "a"(static_cast<uintptr_t>(exnref)),
                     "d"(static_cast<uintptr_t>(0))
                   : "memory");
  __builtin_unreachable();
}

}  // namespace

void unwind_trace(const char* fmt, ...) {
  static unsigned n = 0;
  if (!trace_enabled() || n >= 96) return;
  ++n;
  std::fputs("[unwind] ", stderr);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

bool unwind_init() { return g_table.ready || init_table(); }

// On a worker, returning false makes vmctx.cpp's caller execute the guest's
// `raise -> ud2` path.  That SIGILL lands in whichever process-wide handler was
// installed last, which may restore SIG_DFL and kill the process.  Convert the
// rejection into the worker's own controlled trap instead; on the main thread
// this is a no-op and the honest trap path is preserved.
void reject_worker_throw() { wb::worker_raise_controlled(SIGILL); }

bool unwind_throw(void* vmctx, uint32_t exnref) {
  if (exnref == 0 || !vmctx) {
    reject_worker_throw();
    return false;
  }
  if (!unwind_init()) {
    unwind_trace("exception table unavailable\n");
    reject_worker_throw();
    return false;
  }

  void* store_ptr = nullptr;
  std::memcpy(&store_ptr, static_cast<unsigned char*>(vmctx) + VMCTX_OFF_STORE_CONTEXT,
              sizeof(store_ptr));
  if (!store_ptr) {
    reject_worker_throw();
    return false;
  }
  auto* store = static_cast<VMStoreContext*>(store_ptr);

  // The object's .text begins with _wasm_function_0 (symbol value 0), so the
  // linked address of that export is the code-segment base for table offsets.
  const uintptr_t code_base = reinterpret_cast<uintptr_t>(&_wasm_function_0);

  // These two fields are written by wasmtime_builtin_throw_ref's trampoline
  // before it calls the host builtin (objdump: +0x10 .. +0x38).  They point
  // at the guest try_call return address and that guest frame's FP.
  const uintptr_t exit_pc = store->last_wasm_exit_pc;
  const uintptr_t trampoline_fp = store->last_wasm_exit_trampoline_fp;
  if (!exit_pc || !trampoline_fp) {
    unwind_trace("tid=%ld no exit state (pc=%#lx fp=%#lx)\n", current_tid(),
                 static_cast<unsigned long>(exit_pc), static_cast<unsigned long>(trampoline_fp));
    reject_worker_throw();
    return false;
  }
  const uint64_t rel_pc = exit_pc > code_base ? exit_pc - code_base : 0;
  const uint64_t code_span = g_text_end > code_base
                                 ? g_text_end - code_base
                                 : (uint64_t{1} << 30);
  if (rel_pc == 0 || rel_pc > std::numeric_limits<uint32_t>::max() || rel_pc >= code_span) {
    unwind_trace("tid=%ld invalid exit_pc=%#lx (rel=%#llx span=%#llx); clearing\n",
                 current_tid(), static_cast<unsigned long>(exit_pc),
                 static_cast<unsigned long long>(rel_pc),
                 static_cast<unsigned long long>(code_span));
    reset_wasm_exit_state(vmctx);
    reject_worker_throw();
    return false;
  }
  // A copied store inherits the main thread's FP.  Never dereference a frame
  // pointer that is not on the current pthread's native stack.
  if (!in_current_stack(trampoline_fp)) {
    unwind_trace("tid=%ld exit fp=%#lx is not on this thread's stack (stale copy); clearing\n",
                 current_tid(), static_cast<unsigned long>(trampoline_fp));
    reset_wasm_exit_state(vmctx);
    reject_worker_throw();
    return false;
  }

  uintptr_t fp = *reinterpret_cast<uintptr_t*>(trampoline_fp);  // last_wasm_exit_fp()
  uintptr_t pc = exit_pc;

  unwind_trace("throw tid=%ld exnref=%#x exit_pc=%#lx (rel %#lx) xfp=%#lx fp=%#lx code=%#lx\n",
               current_tid(), exnref, static_cast<unsigned long>(exit_pc),
               static_cast<unsigned long>(rel_pc), static_cast<unsigned long>(trampoline_fp),
               static_cast<unsigned long>(fp), static_cast<unsigned long>(code_base));

  // DRC exception object layout: VMGcHeader (kind/ty) at +0/+4, VMDrcHeader
  // at +8..+23, then the tag's instance id and defined-tag index at +24/+28
  // (confirmed by the bounded object dump in vmctx.cpp: 1, 1).
  uint32_t exn_instance = 0;
  uint32_t exn_defined_tag = 0;
  if (store->gc_heap.base && exnref + 32 <= store->gc_heap.current_length) {
    const unsigned char* obj = static_cast<const unsigned char*>(store->gc_heap.base) + exnref;
    if ((rd32(obj) & 0xfc000000u) == 0x04000000u) {  // VMgcKind::ExnRef
      exn_instance = rd32(obj + 24);
      exn_defined_tag = rd32(obj + 28);
    }
  }
  unwind_trace("exception tag: instance=%u defined=%u\n", exn_instance, exn_defined_tag);

  UnwindHandler chosen{};
  for (unsigned frame = 0; frame < kMaxFrames; ++frame) {
    HandlerRange range{};
    if (!in_current_stack(fp)) {
      unwind_trace("frame %u fp=%#lx (tid=%ld) outside this thread's stack\n", frame,
                   static_cast<unsigned long>(fp), current_tid());
      break;
    }
    if (pc > code_base && lookup_pc(static_cast<uint32_t>(pc - code_base), range)) {
      const uint32_t frame_offset = range.frame_offset;
      // FP points near the top of the stack (e.g. 0x7ffe...); only the
      // *distance* down to SP is bounded, never its absolute address.
      const bool frame_ok = frame_offset != kNoTag && frame_offset < (64u << 20) &&
                            fp > frame_offset;
      if (frame_ok) {
        const uintptr_t sp = fp - frame_offset;
        if (in_current_stack(sp)) {
          for (uint32_t i = range.begin; i < range.end; ++i) {
            const uint32_t tag = g_table.tags[i];
            const uint32_t ctx = g_table.contexts[i];
            const uint32_t target = g_table.handlers[i];
            unwind_trace("frame %u pc=%#lx fp=%#lx sp=%#lx callsite=%u handler[%u] "
                         "tag=%#x ctx=%#x target=%#lx\n",
                         frame, static_cast<unsigned long>(pc), static_cast<unsigned long>(fp),
                         static_cast<unsigned long>(sp), range.index, i, tag, ctx,
                         static_cast<unsigned long>(target));
            if (target == 0 || (g_text_end && code_base + target >= g_text_end)) continue;
            bool matches = (tag == kNoTag);
            if (!matches && exn_instance != 0 && ctx != kNoTag && in_current_stack(sp + ctx)) {
              // Wasmtime resolves a tag handler's module-local index through
              // the vmctx saved in the frame's dynamic context slot.  With one
              // instance, its defined-tag index equals the module-local tag.
              const uintptr_t frame_vmctx = *reinterpret_cast<const uintptr_t*>(sp + ctx);
              if (frame_vmctx == reinterpret_cast<uintptr_t>(vmctx) && tag == exn_defined_tag) {
                matches = true;
              }
            }
            if (!matches) continue;
            chosen.pc = code_base + target;
            chosen.sp = sp;
            chosen.fp = fp;
            chosen.tag = tag;
            unwind_trace("FOUND handler pc=%#lx sp=%#lx fp=%#lx\n",
                         static_cast<unsigned long>(chosen.pc),
                         static_cast<unsigned long>(chosen.sp),
                         static_cast<unsigned long>(chosen.fp));
            resume_to_handler(chosen, exnref);
          }
        } else {
          unwind_trace("frame %u sp=%#lx outside this thread's stack\n", frame,
                       static_cast<unsigned long>(sp));
        }
      } else {
        unwind_trace("frame %u pc=%#lx fp=%#lx: bad frame_offset=%#x\n", frame,
                     static_cast<unsigned long>(pc), static_cast<unsigned long>(fp),
                     frame_offset);
      }
    }
    const uintptr_t next_fp = *reinterpret_cast<uintptr_t*>(fp);
    const uintptr_t next_pc = *reinterpret_cast<uintptr_t*>(fp + 8);
    if (next_fp <= fp || next_fp - fp > (1u << 20) || (next_fp & 15) != 0 || next_pc == 0) break;
    fp = next_fp;
    pc = next_pc;
  }
  unwind_trace("no catch-all handler found; propagating as trap\n");
  reject_worker_throw();  // worker: never reach the guest's raise -> ud2
  return false;
}

}  // namespace host
