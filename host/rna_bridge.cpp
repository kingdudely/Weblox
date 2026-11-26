// Implementation of weblox::RnaBridge. See host/rna_bridge.h and host/RNA.md.
#include "rna_bridge.h"

#include "imports.h"
#include "table.h"
#include "vmctx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Wasmtime 50's wasm-call ABI: rdi=callee vmctx, rsi=caller vmctx, wasm args
// in rdx/rcx/r8/r9 and (for arg 5+) on the stack. Stack arguments are popped
// by the callee (`ret $N`), so the caller must not adjust rsp after `call`.
// The generic shim below takes (callee vmctx, caller vmctx, fn, uint32_t args[6],
// uint32_t n) and never passes more than six i32 arguments, which covers every
// RNA callback (the largest is on_connection_closed: fnid + 4 args).
extern "C" int64_t wb_rna_invoke(void* callee_vmctx, void* caller_vmctx,
                                 void* fn, const uint32_t* args, uint32_t n);
asm(
    ".text\n"
    ".globl wb_rna_invoke\n"
    ".type wb_rna_invoke,@function\n"
    "wb_rna_invoke:\n"
    "  push %rbp\n"
    "  mov %rsp, %rbp\n"
    "  push %rbx\n"
    "  push %r12\n"
    "  push %r13\n"
    "  push %r14\n"
    "  push %r15\n"
    "  mov %rdi, %r12\n"          // callee vmctx
    "  mov %rsi, %r13\n"          // caller vmctx
    "  mov %rdx, %r10\n"          // target wasm function
    "  mov %rcx, %r14\n"          // args array
    "  mov %r8d, %r15d\n"         // argument count (<= 6)
    "  mov 0(%r14), %rdx\n"
    "  mov 8(%r14), %rcx\n"
    "  mov 16(%r14), %r8\n"
    "  mov 24(%r14), %r9\n"
    "  mov %r15d, %ebx\n"
    "1:\n"
    "  decl %ebx\n"
    "  cmpl $4, %ebx\n"
    "  jb 2f\n"
    "  mov 0(%r14,%rbx,8), %rax\n"
    "  push %rax\n"
    "  jmp 1b\n"
    "2:\n"
    "  mov %r12, %rdi\n"
    "  mov %r13, %rsi\n"
    "  call *%r10\n"              // callee pops any stack arguments
    "  pop %r15\n"
    "  pop %r14\n"
    "  pop %r13\n"
    "  pop %r12\n"
    "  pop %rbx\n"
    "  pop %rbp\n"
    "  ret\n");

extern "C" {
// malloc/free exports of the guest (wasm indices 150325 / 150327).
uint32_t _wasm_function_149984(void*, void*, uint32_t);
void _wasm_function_149986(void*, void*, uint32_t);
}

namespace weblox {
namespace {

constexpr uint32_t kConsumerTableIndex = 135389;  // engine per-slot consumer
constexpr uint32_t kRnaRingDataInConn = 336;      // from v2-wasmtime/src/imports/rna.rs

enum class EvKind {
  kConnectSuccess,
  kConnectError,
  kStreamAccepted,
  kRingSlot,
  kStreamFin,
  kStreamError,
  kWriteSuccess,
  kWriteError,
  kClosed,
  kCloseComplete,
  kDatagramAcked,
  kDatagramLost,
};

struct Event {
  EvKind kind = EvKind::kConnectError;
  uint32_t id = 0;
  uint32_t a = 0, b = 0, c = 0, d = 0;
};

const char* ev_name(EvKind k) {
  switch (k) {
    case EvKind::kConnectSuccess: return "rbx_wasm_rna_on_connect_success";
    case EvKind::kConnectError: return "rbx_wasm_rna_on_connect_error";
    case EvKind::kStreamAccepted: return "rbx_wasm_rna_on_stream_accepted";
    case EvKind::kStreamFin: return "rbx_wasm_rna_on_stream_fin";
    case EvKind::kStreamError: return "rbx_wasm_rna_on_stream_error";
    case EvKind::kWriteSuccess: return "rbx_wasm_rna_on_stream_write_success";
    case EvKind::kWriteError: return "rbx_wasm_rna_on_stream_write_error";
    case EvKind::kClosed: return "rbx_wasm_rna_on_connection_closed";
    case EvKind::kCloseComplete: return "rbx_wasm_rna_on_close_complete";
    case EvKind::kDatagramAcked: return "rbx_wasm_rna_on_datagram_acked";
    case EvKind::kDatagramLost: return "rbx_wasm_rna_on_datagram_lost";
    case EvKind::kRingSlot: return "rbx_wasm_rna_on_receive_ring_slot";
  }
  return "?";
}

std::string url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    if (s[i] == '+') out.push_back(' ');
    else out.push_back(s[i]);
  }
  return out;
}

// The guest URL is (from the live page):
//   https://<game-fqdn>:<port>/wt?X%2DRbx%2DCapabilities=<hex>&X-Rbx-Close-Reason=...
// Only the FQDN, port and caps are transport inputs; the UDP peer is the
// browser .34 address from the join route, never the FQDN in the URL.
bool parse_wt_url(const std::string& url, std::string* fqdn, uint16_t* port,
                  std::string* caps, std::string* error) {
  size_t p = 0;
  if (url.rfind("https://", 0) == 0) p = 8;
  else if (url.rfind("wss://", 0) == 0) p = 6;
  else {
    *error = "unsupported WT URL scheme";
    return false;
  }
  const size_t slash = url.find('/', p);
  const std::string authority = url.substr(p, slash == std::string::npos
                                                 ? std::string::npos
                                                 : slash - p);
  const size_t colon = authority.rfind(':');
  if (colon == std::string::npos || colon == 0) {
    *error = "WT URL has no port";
    return false;
  }
  // `colon` is an index into `authority`, not into the whole URL.
  *fqdn = url.substr(p, colon);
  const unsigned long parsed = std::strtoul(authority.c_str() + colon + 1, nullptr, 10);
  if (parsed == 0 || parsed > 65535) {
    *error = "bad WT URL port";
    return false;
  }
  *port = static_cast<uint16_t>(parsed);
  caps->clear();
  const size_t q = url.find('?');
  if (q != std::string::npos) {
    const std::string query = url.substr(q + 1);
    size_t at = 0;
    while (at < query.size()) {
      const size_t amp = query.find('&', at);
      const std::string pair = query.substr(at, amp == std::string::npos
                                                  ? std::string::npos
                                                  : amp - at);
      const size_t eq = pair.find('=');
      if (eq != std::string::npos &&
          url_decode(pair.substr(0, eq)) == "X-Rbx-Capabilities") {
        *caps = url_decode(pair.substr(eq + 1));
      }
      if (amp == std::string::npos) break;
      at = amp + 1;
    }
  }
  if (fqdn->empty()) {
    *error = "empty WT FQDN";
    return false;
  }
  return true;
}

struct Conn {
  uint32_t id = 0;
  std::unique_ptr<WebTransport> wt;
  RnaRing ring;
  uint32_t error_out = 0;
  std::mutex mu;                  // serialises guest calls and the pumper
  std::atomic<bool> stop{false};
  std::thread pumper;
  std::unordered_map<uint64_t, int32_t> streams;  // quic id -> RNA stream id
  std::unordered_map<int32_t, uint64_t> reverse;  // RNA stream id -> quic id
  std::unordered_map<int32_t, std::string> pending;  // writes before open
  std::set<int32_t> paused;
  std::set<int32_t> stopped_read;
  int32_t next_stream_id = 3;     // page starts server-initiated ids at 3, +=4
  int64_t last_status = 0;
  std::string error;
};

}  // namespace

struct RnaBridge::Impl {
  Config config;
  RnaBridge* owner = nullptr;
  host::HostState* state = nullptr;
  uint8_t* mem = nullptr;
  size_t mem_len = 0;
  std::mutex conn_mu;
  std::unordered_map<uint32_t, std::unique_ptr<Conn>> conns;
  std::mutex ev_mu;
  std::deque<Event> events;
  std::atomic<bool> in_deliver{false};
  std::atomic<bool> verbose{true};
  RnaStats counters;
  std::unordered_set<std::string> missing_bindings;
  static Impl* singleton;

  void log(const std::string& line) const {
    if (verbose.load(std::memory_order_relaxed)) {
      std::fprintf(stderr, "[rna ] %s\n", line.c_str());
    }
  }

  // ---- guest linear memory helpers ---------------------------------------
  bool range(uint32_t p, size_t n) const {
    return mem && p <= mem_len && n <= mem_len - static_cast<size_t>(p);
  }
  uint32_t ld32(uint32_t p) const {
    if (!range(p, 4)) return 0;
    uint32_t v = 0;
    std::memcpy(&v, mem + p, 4);
    return v;
  }
  void st32(uint32_t p, uint32_t v) const {
    if (!range(p, 4)) return;
    std::memcpy(mem + p, &v, 4);
  }
  std::string cstr(uint32_t p, size_t cap = 4096) const {
    if (!range(p, 1)) return {};
    const size_t max = std::min(cap, mem_len - static_cast<size_t>(p));
    const void* end = std::memchr(mem + p, 0, max);
    const size_t n = end ? static_cast<const uint8_t*>(end) - (mem + p) : max;
    return std::string(reinterpret_cast<const char*>(mem + p), n);
  }
  std::vector<uint8_t> bytes(uint32_t p, uint32_t n) const {
    if (!range(p, n)) return {};
    return std::vector<uint8_t>(mem + p, mem + p + n);
  }

  // ---- completion queue --------------------------------------------------
  void push(Event ev) {
    {
      std::lock_guard<std::mutex> lock(ev_mu);
      if (events.size() < 8192) events.push_back(ev);
    }
    // The page wakes the owner's_asyncify futex at every completion. We do not
    // know the guest's owner word; wake both ring cursors as a best effort.
    if (ev.id) {
      std::lock_guard<std::mutex> lock(conn_mu);
      auto it = conns.find(ev.id);
      if (it != conns.end() && it->second->ring.head) {
        wb::imports_futex_notify(it->second->ring.head, 1);
        wb::imports_futex_notify(it->second->ring.tail, 1);
      }
    }
  }

  // ---- guest callback re-entry (guest thread only) -----------------------
  bool invoke_table(uint32_t table_index, const uint32_t* args, uint32_t n) {
    if (!state || table_index >= wb_table_size) return false;
    host::VMFuncRef& fr = state->table_funcrefs[table_index];
    if (!fr.wasm_call) return false;
    uint32_t a[6] = {0, 0, 0, 0, 0, 0};
    const uint32_t count = std::min<uint32_t>(n, 6);
    for (uint32_t i = 0; i < count; ++i) a[i] = args[i];
    wb_rna_invoke(fr.vmctx, state->vmctx, fr.wasm_call, a, count);
    return true;
  }

  bool invoke_binding(const char* name, const uint32_t* args, uint32_t n) {
    const wb::Binding* b = wb::embind_find(name);
    if (!b) {
      if (missing_bindings.insert(name).second) {
        log(std::string("no embind binding for ") + name);
      }
      return false;
    }
    uint32_t a[6] = {b->id, 0, 0, 0, 0, 0};
    const uint32_t count = std::min<uint32_t>(n + 1, 6u);
    for (uint32_t i = 0; i < count - 1; ++i) a[i + 1] = args[i];
    return invoke_table(b->invoker, a, count);
  }

  // The engine's per-slot consumer (fn 120561 in the Rust port) advances the
  // read cursor and dispatches the payload. Its page wrapper only queues the
  // same call for the deferred callback thread, which this host does not have,
  // so call the consumer directly. Task layout: {conn, kind, sid, pad, slot, len}.
  bool consume_ring_slot(const Event& ev) {
    if (!state || !mem) return false;
    const uint32_t task = _wasm_function_149984(state->vmctx, state->vmctx, 24);
    if (!task) return false;
    st32(task + 0, static_cast<uint32_t>(ev.a) - kRnaRingDataInConn);
    st32(task + 4, ev.b);
    st32(task + 8, ev.c);
    st32(task + 12, 0);
    st32(task + 16, ev.d >> 8);
    st32(task + 20, ev.d & 0xff);
    const uint32_t args[1] = {task};
    const bool ok = invoke_table(kConsumerTableIndex, args, 1);
    _wasm_function_149986(state->vmctx, state->vmctx, task);
    return ok;
  }

  void deliver_one() {
    if (in_deliver.exchange(true, std::memory_order_acq_rel)) return;
    Event ev;
    {
      std::lock_guard<std::mutex> lock(ev_mu);
      if (!events.empty()) {
        ev = events.front();
        events.pop_front();
      }
    }
    if (ev.id || ev.kind != EvKind::kConnectError || ev.a != 0) {
      bool ok = false;
      if (ev.kind == EvKind::kRingSlot) {
        ok = consume_ring_slot(ev);
        if (!ok) {
          const uint32_t args[5] = {ev.id, ev.b, ev.c, ev.d >> 8, ev.d & 0xff};
          ok = invoke_binding(ev_name(ev.kind), args, 5);
        }
      } else {
        const uint32_t args[5] = {ev.id, ev.a, ev.b, ev.c, ev.d};
        uint32_t n = 4;
        switch (ev.kind) {
          case EvKind::kConnectSuccess: n = 2; break;
          case EvKind::kConnectError: n = 1; break;
          case EvKind::kStreamAccepted:
          case EvKind::kStreamFin:
          case EvKind::kWriteError: n = 2; break;
          case EvKind::kStreamError:
          case EvKind::kWriteSuccess: n = 3; break;
          case EvKind::kClosed: n = 5; break;  // id, code, class, has-state
          case EvKind::kDatagramAcked:
          case EvKind::kDatagramLost: n = 4; break;
          case EvKind::kCloseComplete: n = 1; break;
          case EvKind::kRingSlot: break;
        }
        ok = invoke_binding(ev_name(ev.kind), args, n);
      }
      if (ok) ++counters.callbacks_delivered;
      else ++counters.callbacks_failed;
      log(std::string("-> ") + ev_name(ev.kind) + " id=" + std::to_string(ev.id));
    }
    in_deliver.store(false, std::memory_order_release);
  }

  // ---- receive ring -------------------------------------------------------
  // Mirrors WebClient.pretty.js::tryWriteReceiveRingSlot / rna.rs::ring_write.
  bool ring_write(uint32_t id, const uint8_t* data, size_t len,
                  uint32_t* slot_out) {
    RnaRing ring{};
    {
      std::lock_guard<std::mutex> lock(conn_mu);
      auto it = conns.find(id);
      if (it == conns.end()) return false;
      ring = it->second->ring;
    }
    if (!mem || ring.count == 0 || ring.slot == 0 || ring.data == 0) return false;
    if (ring.count > (1u << 20) || ring.slot > (1u << 20)) return false;
    const uint64_t bytes = static_cast<uint64_t>(ring.count) * ring.slot;
    if (!range(ring.data, static_cast<size_t>(bytes))) return false;
    if ((ring.head | ring.tail) % 4) return false;
    volatile uint32_t* head = reinterpret_cast<volatile uint32_t*>(mem + ring.head);
    volatile uint32_t* tail = reinterpret_cast<volatile uint32_t*>(mem + ring.tail);
    const uint32_t h = __atomic_load_n(head, __ATOMIC_SEQ_CST);
    const uint32_t t = __atomic_load_n(tail, __ATOMIC_SEQ_CST);
    if (h - t >= ring.count) {
      ++counters.ring_drops;
      return false;
    }
    const uint32_t index = h % ring.count;
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(len, ring.slot));
    std::memcpy(mem + ring.data + static_cast<size_t>(index) * ring.slot,
                data, n);
    __atomic_store_n(head, h + 1, __ATOMIC_SEQ_CST);
    *slot_out = index;
    ++counters.ring_slots_written;
    return true;
  }

  // ---- native connect (shared by guest connect_js and the selftest) -------
  Conn* make_conn(uint32_t id, const RnaRing& ring, uint32_t error_out,
                  std::string* error) {
    if (config.host.empty() || config.port == 0) {
      *error = "no transport host/port (call set_config with a JoinRoute)";
      return nullptr;
    }
    std::unique_ptr<Conn> conn = std::make_unique<Conn>();
    conn->id = id;
    conn->ring = ring;
    conn->error_out = error_out;
    conn->wt = std::make_unique<WebTransport>();
    WebTransport::Options opts;
    opts.connect_timeout_ms = config.connect_timeout_ms;
    opts.require_cert_hash = !config.cert_hash.empty();
    conn->wt->set_status_callback([conn_ptr = conn.get()](
                                      Status status, const std::string& msg) {
      conn_ptr->error = msg;
      if (status == Status::kError) {
        std::fprintf(stderr, "[rna ] wt status error: %s\n", msg.c_str());
      }
    });
    const std::string cert = config.cert_hash.empty()
                                 ? config.cert_hash
                                 : config.cert_hash;
    if (!conn->wt->connect(config.host, config.port, config.fqdn, config.caps,
                           cert, opts)) {
      *error = conn->wt->last_error();
      return nullptr;
    }
    conn->last_status = conn->wt->status_code();
    Conn* out = conn.get();
    std::lock_guard<std::mutex> lock(conn_mu);
    conns[id] = std::move(conn);
    return out;
  }

  void start_pumper(Conn* conn) {
    const uint32_t id = conn->id;
    conn->pumper = std::thread([this, id] { pump_loop(id); });
  }

  void pump_loop(uint32_t id) {
    std::vector<uint8_t> dgram;
    std::vector<StreamMessage> msgs;
    while (true) {
      Conn* c = nullptr;
      {
        std::lock_guard<std::mutex> lock(conn_mu);
        auto it = conns.find(id);
        if (it == conns.end()) break;
        c = it->second.get();
      }
      if (c->stop.load(std::memory_order_acquire)) break;
      bool alive = true;
      {
        std::lock_guard<std::mutex> lock(c->mu);
        alive = c->wt->pump();
        while (c->wt->recv_datagram(&dgram)) {
          ++counters.datagrams_recv;
          uint32_t slot = 0;
          if (ring_write(id, dgram.data(), dgram.size(), &slot)) {
            // packed slot/len as in rna.rs: (slot << 24) | len
            push({EvKind::kRingSlot, id, 0, 1, 0,
                  (slot << 24) | std::min<uint32_t>(dgram.size(), 0xffffff)});
          }
        }
        c->wt->drain_streams(&msgs);
      }
      for (const StreamMessage& m : msgs) {
        int32_t sid = 0;
        {
          std::lock_guard<std::mutex> lock(c->mu);
          auto it = c->streams.find(m.stream_id);
          if (it == c->streams.end()) {
            sid = c->next_stream_id;
            c->next_stream_id += 4;
            c->streams[m.stream_id] = sid;
            c->reverse[sid] = m.stream_id;
            push({EvKind::kStreamAccepted, id, static_cast<uint32_t>(sid), 0, 0, 0});
          } else {
            sid = it->second;
          }
        }
        if (c->paused.count(sid) || c->stopped_read.count(sid)) continue;
        uint32_t slot = 0;
        if (ring_write(id, m.data.data(), m.data.size(), &slot)) {
          push({EvKind::kRingSlot, id, static_cast<uint32_t>(sid), 0, 0,
                (slot << 24) | std::min<uint32_t>(m.data.size(), 0xffffff)});
        }
        if (m.fin) push({EvKind::kStreamFin, id, static_cast<uint32_t>(sid), 0, 0, 0});
      }
      msgs.clear();
      if (!alive) {
        push({EvKind::kClosed, id, 0, 0, 0, 0});
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
};

RnaBridge::Impl* RnaBridge::Impl::singleton = nullptr;

RnaBridge::RnaBridge() : impl_(std::make_unique<Impl>()) {
  impl_->owner = this;
  Impl::singleton = impl_.get();
}
RnaBridge::~RnaBridge() {
  stop_all();
  if (Impl::singleton == impl_.get()) Impl::singleton = nullptr;
}

RnaBridge* active_bridge() {
  return RnaBridge::Impl::singleton ? RnaBridge::Impl::singleton->owner : nullptr;
}

void rna_guest_tick(void* /*vmctx*/) {
  if (RnaBridge::Impl::singleton) RnaBridge::Impl::singleton->deliver_one();
}

void RnaBridge::attach(host::HostState& state) {
  impl_->state = &state;
  impl_->mem = state.memory_base;
  impl_->mem_len = state.memory_def_storage.current_length;
  wb::imports_set_guest_tick(&rna_guest_tick);
  impl_->log("attached: memory=" + std::to_string(impl_->mem_len) +
             " bytes table=" + std::to_string(wb_table_size) + " entries");
}

void RnaBridge::set_config(const Config& config) { impl_->config = config; }
void RnaBridge::set_config(const JoinRoute& route) {
  impl_->config.host = route.host;
  impl_->config.port = route.port;
  impl_->config.fqdn = route.fqdn;
  impl_->config.caps = route.caps;
  impl_->config.cert_hash = route.cert_hash;
}

void RnaBridge::connect(const RnaConnectArgs& args) {
  std::string error;
  std::string fqdn;
  uint16_t port = 0;
  std::string caps;
  if (!parse_wt_url(args.url, &fqdn, &port, &caps, &error)) {
    impl_->log("connect id=" + std::to_string(args.id) + " rejected: " + error);
    impl_->push({EvKind::kConnectError, args.id, 0, 0, 0, 0});
    if (args.error_out) impl_->st32(args.error_out, 1);
    ++impl_->counters.connect_errors;
    impl_->counters.last_error = error;
    return;
  }
  Config cfg = impl_->config;
  // The route normally matches the URL; when it does not, prefer the URL for
  // SNI/:authority/caps but keep the route's UDP peer (the URL has no IP).
  if (!fqdn.empty() && !cfg.fqdn.empty() && fqdn != cfg.fqdn) {
    impl_->log("URL FQDN differs from route FQDN; using URL FQDN (url=" +
               std::to_string(fqdn.size()) + " route=" +
               std::to_string(cfg.fqdn.size()) + ")");
  }
  cfg.fqdn = fqdn;
  cfg.port = port;
  if (!caps.empty()) cfg.caps = caps;
  if (!args.cert_hashes.empty() && !args.cert_hashes[0].empty()) {
    cfg.cert_hash = args.cert_hashes[0];
  }
  const bool previous_verbose = impl_->verbose.load();
  impl_->config = cfg;
  impl_->log("connect id=" + std::to_string(args.id) + " url=" + args.url +
             " caps=" + cfg.caps + " certs=" + std::to_string(args.cert_hashes.size()) +
             " ring(data=" + std::to_string(args.ring.data) +
             ",head=" + std::to_string(args.ring.head) +
             ",tail=" + std::to_string(args.ring.tail) +
             ",slot=" + std::to_string(args.ring.slot) +
             ",count=" + std::to_string(args.ring.count) + ")");
  (void)previous_verbose;

  Conn* conn = impl_->make_conn(args.id, args.ring, args.error_out, &error);
  if (!conn) {
    impl_->log("connect failed: " + error);
    impl_->push({EvKind::kConnectError, args.id, 0, 0, 0, 0});
    if (args.error_out) impl_->st32(args.error_out, 1);
    ++impl_->counters.connect_errors;
    impl_->counters.last_error = error;
    return;
  }
  ++impl_->counters.connects;
  impl_->counters.connect_status = conn->last_status;
  // datagrams.maxDatagramSize in the browser is typically 1200; WebTransport
  // has no getter in this host, so report the conservative QUIC-safe value.
  impl_->push({EvKind::kConnectSuccess, args.id, 1200, 0, 0, 0});
  impl_->start_pumper(conn);
}

void RnaBridge::send_datagram(uint32_t id, uint32_t ptr, uint32_t len,
                              uint32_t hi, uint32_t lo) {
  const std::vector<uint8_t> payload = impl_->bytes(ptr, len);
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (!c) return;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(c->mu);
    // wt::send_datagram() adds the qdmux session-id varint itself.
    ok = c->wt->send_datagram(payload);
  }
  if (ok) {
    ++impl_->counters.datagrams_sent;
    impl_->push({EvKind::kDatagramAcked, id, hi, lo, 0, 0});
  } else {
    impl_->push({EvKind::kDatagramLost, id, hi, lo, 0, 0});
  }
}

void RnaBridge::open_stream(uint32_t id, uint32_t sid) {
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (!c) return;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(c->mu);
    auto it = c->reverse.find(static_cast<int32_t>(sid));
    if (it == c->reverse.end()) {
      const int64_t stream = c->wt->open_uni_stream();  // WT_UNI header by wt.cpp
      if (stream >= 0) {
        c->streams[static_cast<uint64_t>(stream)] = static_cast<int32_t>(sid);
        c->reverse[static_cast<int32_t>(sid)] = static_cast<uint64_t>(stream);
        ++impl_->counters.streams_opened;
        ok = true;
      }
    } else {
      ok = true;
    }
    // The engine can write on the same tick it opens a lane; replay anything
    // that arrived before the QUIC stream existed.
    if (ok) {
      auto pending = c->pending.find(static_cast<int32_t>(sid));
      if (pending != c->pending.end()) {
        const bool sent = pending->second.empty() ||
                          c->wt->send_stream_data(c->reverse[static_cast<int32_t>(sid)],
                                                  std::vector<uint8_t>(
                                                      pending->second.begin(),
                                                      pending->second.end()));
        impl_->push({sent ? EvKind::kWriteSuccess : EvKind::kWriteError,
                     id, sid,
                     sent ? static_cast<uint32_t>(pending->second.size()) : 0u,
                     0, 0});
        c->pending.erase(pending);
      }
    }
  }
  if (!ok) {
    impl_->push({EvKind::kWriteError, id, sid, 0, 0, 0});
  }
}

void RnaBridge::write_stream(uint32_t id, uint32_t sid, uint32_t ptr,
                             uint32_t len) {
  std::vector<uint8_t> data = impl_->bytes(ptr, len);
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (!c) return;
  {
    std::lock_guard<std::mutex> lock(c->mu);
    auto it = c->reverse.find(static_cast<int32_t>(sid));
    if (it == c->reverse.end() || data.empty()) {
      // Writes on the same tick as open_stream_js are queued by the page.
      auto& q = c->pending[static_cast<int32_t>(sid)];
      q.append(reinterpret_cast<const char*>(data.data()), data.size());
      return;
    }
    const bool ok = c->wt->send_stream_data(it->second, data);
    if (ok) {
      ++impl_->counters.stream_writes;
      impl_->push({EvKind::kWriteSuccess, id, sid,
                   static_cast<uint32_t>(data.size()), 0, 0});
    } else {
      impl_->push({EvKind::kWriteError, id, sid, 0, 0, 0});
    }
  }
}

void RnaBridge::shutdown_stream_write(uint32_t id, uint32_t sid) {
  // wt::send_stream_data() has no FIN argument; there is no honest way to
  // finish a WebTransport stream with the current native API. Log and leave
  // the stream open rather than pretending the peer saw a FIN.
  impl_->log("shutdown_stream_write id=" + std::to_string(id) +
             " sid=" + std::to_string(sid) + ": stub (no FIN in wt.cpp)");
}
void RnaBridge::shutdown_stream_read(uint32_t id, uint32_t sid) {
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (c) c->stopped_read.insert(static_cast<int32_t>(sid));
}
void RnaBridge::pause_stream_read(uint32_t id, uint32_t sid) {
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (c) c->paused.insert(static_cast<int32_t>(sid));
}
void RnaBridge::resume_stream_read(uint32_t id, uint32_t sid) {
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (c) c->paused.erase(static_cast<int32_t>(sid));
}
void RnaBridge::receive_ring_consumed(uint32_t id, uint32_t kind, uint32_t sid) {
  (void)kind;
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (c) c->paused.erase(static_cast<int32_t>(sid));
}
void RnaBridge::close(uint32_t id) {
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (!c) {
    impl_->push({EvKind::kCloseComplete, id, 0, 0, 0, 0});
    return;
  }
  c->stop.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(c->mu);
    c->wt->close();
  }
  impl_->push({EvKind::kCloseComplete, id, 0, 0, 0, 0});
}
void RnaBridge::send_connection_close(uint32_t id) {
  Conn* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    auto it = impl_->conns.find(id);
    if (it != impl_->conns.end()) c = it->second.get();
  }
  if (!c) return;
  c->stop.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lock(c->mu);
  c->wt->close();
}
int32_t RnaBridge::is_supported() const { return 1; }

void RnaBridge::guest_tick() { impl_->deliver_one(); }

void RnaBridge::stop_all() {
  std::unordered_map<uint32_t, Conn*> snapshot;
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    for (auto& kv : impl_->conns) snapshot[kv.first] = kv.second.get();
  }
  for (auto& kv : snapshot) {
    Conn* c = kv.second;
    c->stop.store(true, std::memory_order_release);
    if (c->pumper.joinable()) c->pumper.join();
    std::lock_guard<std::mutex> lock(c->mu);
    c->wt->close();
  }
  {
    std::lock_guard<std::mutex> lock(impl_->conn_mu);
    impl_->conns.clear();
  }
}

bool RnaBridge::selftest(uint32_t seconds, std::string* report) {
  const RnaRing empty{};
  std::string error;
  Conn* conn = impl_->make_conn(1, empty, 0, &error);
  if (!conn) {
    if (report) *report = "CONNECT failed: " + error;
    return false;
  }
  ++impl_->counters.connects;
  impl_->counters.connect_status = conn->last_status;
  impl_->start_pumper(conn);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  const uint64_t sent = conn->wt->packets_sent();
  const uint64_t recv = conn->wt->packets_recv();
  const uint64_t pings = conn->wt->keepalives_sent();
  const bool open = conn->wt->is_open();
  stop_all();
  if (report) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "status=%lld open=%d packets_sent=%llu packets_recv=%llu "
                  "pings=%llu ring_slots=%llu ring_drops=%llu callbacks=%llu/%llu",
                  static_cast<long long>(impl_->counters.connect_status),
                  open ? 1 : 0, static_cast<unsigned long long>(sent),
                  static_cast<unsigned long long>(recv),
                  static_cast<unsigned long long>(pings),
                  static_cast<unsigned long long>(impl_->counters.ring_slots_written),
                  static_cast<unsigned long long>(impl_->counters.ring_drops),
                  static_cast<unsigned long long>(impl_->counters.callbacks_delivered),
                  static_cast<unsigned long long>(impl_->counters.callbacks_failed));
    *report = buf;
  }
  return impl_->counters.connect_status == 200;
}

RnaStats RnaBridge::stats() const { return impl_->counters; }

}  // namespace weblox
