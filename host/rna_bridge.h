// Glue between the wasm `rbx_wasm_rna_*` imports and weblox::WebTransport.
//
// The page implementation of these imports wraps the browser WebTransport and
// delivers its completions on the engine's main thread via a MessageChannel.
// This host has no browser event loop, so the bridge mirrors that protocol:
//
//   * connect_js / send_datagram / write_stream etc. run on the guest thread
//     and call the native WebTransport directly (serialised with the pumper);
//   * a pumper thread drives WebTransport::pump(), writes received payloads
//     into the guest's shared receive rings and queues completions;
//   * completions are delivered back into wasm from a host hook fired on the
//     guest thread (wb::imports_set_guest_tick), never from the pumper thread.
//
// See host/RNA.md for the build flags, the protocol details and the evidence.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "route.h"
#include "wt.h"

namespace host {
struct HostState;
}

namespace weblox {

struct RnaRing {
  uint32_t data = 0;
  uint32_t head = 0;
  uint32_t tail = 0;
  uint32_t slot = 0;
  uint32_t count = 0;
};

// Decoded arguments of slot 96 (rbx_wasm_rna_connect_js). The wasm import has
// 14 i32 parameters; the page glue consumes the first 12 (see
// docs/reference/research/WebClient.pretty.js, function oa).
struct RnaConnectArgs {
  uint32_t id = 0;
  std::string url;
  std::vector<std::string> cert_hashes;  // base64 SHA-256, guest-supplied
  RnaRing ring;
  uint32_t error_out = 0;   // a9: guest error word, optional
  bool fix_ownership = false;
  bool datagram_compat = false;
};

struct RnaStats {
  uint64_t connects = 0;
  uint64_t connect_errors = 0;
  uint64_t datagrams_sent = 0;
  uint64_t datagrams_recv = 0;
  uint64_t ring_slots_written = 0;
  uint64_t ring_drops = 0;
  uint64_t streams_opened = 0;
  uint64_t stream_writes = 0;
  uint64_t callbacks_delivered = 0;
  uint64_t callbacks_failed = 0;
  int64_t connect_status = 0;  // HTTP/3 :status of the last connect
  std::string last_error;
};

class RnaBridge {
 public:
  // Route-derived transport inputs. The RNA URL carries the qdmux FQDN, the
  // port and the caps; the UDP peer (.34 address) and certificate pin come
  // from the gamejoin route (or from environment overrides in selftest mode).
  struct Config {
    std::string host;
    uint16_t port = 0;
    std::string fqdn;
    std::string caps;
    std::string cert_hash;
    uint64_t connect_timeout_ms = 15000;
    bool verbose = true;
  };

  RnaBridge();
  ~RnaBridge();
  RnaBridge(const RnaBridge&) = delete;
  RnaBridge& operator=(const RnaBridge&) = delete;

  // Bind guest linear memory / table / vmctx and install the guest-tick hook.
  void attach(host::HostState& state);
  void set_config(const Config& config);
  void set_config(const JoinRoute& route);

  // Slots 86..97. These are called from the wasm import stubs on the guest
  // thread; they never re-enter wasm.
  void connect(const RnaConnectArgs& args);
  void send_datagram(uint32_t id, uint32_t ptr, uint32_t len,
                     uint32_t hi, uint32_t lo);
  void open_stream(uint32_t id, uint32_t sid);
  void write_stream(uint32_t id, uint32_t sid, uint32_t ptr, uint32_t len);
  void shutdown_stream_write(uint32_t id, uint32_t sid);
  void shutdown_stream_read(uint32_t id, uint32_t sid);
  void pause_stream_read(uint32_t id, uint32_t sid);
  void resume_stream_read(uint32_t id, uint32_t sid);
  void receive_ring_consumed(uint32_t id, uint32_t kind, uint32_t sid);
  void close(uint32_t id);
  void send_connection_close(uint32_t id);
  int32_t is_supported() const;

  // Called on the guest thread from imports.cpp; delivers at most one queued
  // completion per call, just like one turn of the page's callback queue.
  void guest_tick();

  // Native-only smoke test: one CONNECT + pump window, no wasm re-entry.
  // Returns true when CONNECT returned 200; `report` is a one-line summary.
  bool selftest(uint32_t seconds, std::string* report);

  void stop_all();
  RnaStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend void rna_guest_tick(void* vmctx);
  friend RnaBridge* active_bridge();
};

// Handler installed into wb::imports_set_guest_tick() by attach().
void rna_guest_tick(void* vmctx);
// The single live bridge (nullptr before construction / after destruction).
RnaBridge* active_bridge();

}  // namespace weblox
