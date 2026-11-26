// Self-contained C++17 WebTransport client for the native host.
//
// This is a thin wrapper over libquiche's C API (quiche.h, quiche 0.30.1). It
// reproduces the protocol proven by tools/quiche-probe/src/main.rs:
//
//   QUIC v1 (ALPN h3) -> H3 SETTINGS (extended CONNECT + draft-02 WT settings)
//   -> extended CONNECT /wt -> :status 200 -> qdmux datagrams/streams.
//
// Build and link instructions: host/WT.md. The implementation is entirely in
// wt.cpp; this header has no quiche includes and can be used in host code as
// `weblox::WebTransport` (the host's generated code uses the separate `wb`
// namespace).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace weblox {

enum class Status {
  kIdle,
  kConnecting,
  kConnected,
  kClosed,
  kError,
};

struct StreamMessage {
  uint64_t stream_id = 0;
  bool unidirectional = false;
  std::vector<uint8_t> data;
  bool fin = false;
};

class WebTransport {
 public:
  struct Options {
    // Deadline for QUIC handshake + CONNECT :status.
    uint64_t connect_timeout_ms = 15'000;
    // QUIC PING cadence. Must stay well below the effective idle timeout.
    uint64_t keepalive_ms = 5'000;
    // Local max_idle_timeout; the effective value is min(local, peer).
    uint64_t idle_timeout_ms = 60'000;
    // If false, an empty cert_hash is accepted without a pin check.
    bool require_cert_hash = true;
    // UDP payload size, matching the probe (1472).
    size_t max_datagram_size = 1472;
  };

  using LogFn = std::function<void(const std::string&)>;
  using StatusFn = std::function<void(Status, const std::string&)>;
  using DatagramFn = std::function<void(std::vector<uint8_t>)>;
  using StreamFn = std::function<void(const StreamMessage&)>;

  WebTransport();
  ~WebTransport();
  WebTransport(const WebTransport&) = delete;
  WebTransport& operator=(const WebTransport&) = delete;

  // Blocks until the extended CONNECT returns :status 200 (or fails/times out).
  // `host` is the UDP peer (browser/UDMUX .34 address), never the qdmux FQDN;
  // `fqdn` is used for TLS SNI and :authority. `cert_hash` is the base64
  // SHA-256 of the route's certHashes[0]; empty means the pin is skipped
  // (logged as a warning) and `verify_cert_hash=false` disables it explicitly.
  bool connect(const std::string& host, uint16_t port,
               const std::string& fqdn, const std::string& caps,
               const std::string& cert_hash);
  bool connect(const std::string& host, uint16_t port,
               const std::string& fqdn, const std::string& caps,
               const std::string& cert_hash, const Options& options);

  void set_log_callback(LogFn fn);
  void set_status_callback(StatusFn fn);
  void set_datagram_callback(DatagramFn fn);
  void set_stream_callback(StreamFn fn);

  // Drive I/O for up to `duration`; returns false when the connection closed.
  // Call from the host's main loop (or use run_for() for a blocking wait).
  bool run_for(std::chrono::milliseconds duration);
  bool pump();

  // qdmux WebTransport DATAGRAMs: payload prefixed with the session-id varint
  // (the CONNECT stream id). Receive strips the prefix and verifies it.
  bool send_datagram(std::vector<uint8_t> payload);
  bool recv_datagram(std::vector<uint8_t>* payload);

  // WebTransport stream basics. Returned ids are caller-owned QUIC stream ids;
  // the WT_UNI/WT_BI type byte + session-id varint header is written for you.
  int64_t open_uni_stream();
  int64_t open_bi_stream();
  bool send_stream_data(uint64_t stream_id, const std::vector<uint8_t>& data);
  // Drain buffered bytes from server-initiated WT streams.
  size_t drain_streams(std::vector<StreamMessage>* out);

  void close();
  bool is_open() const;
  uint64_t session_id() const;          // CONNECT stream id, or kNoSession.
  static constexpr uint64_t kNoSession = UINT64_MAX;
  int status_code() const;              // HTTP/3 :status, 0 before response.
  bool cert_pin_verified() const;
  uint64_t keepalives_sent() const;
  uint64_t packets_sent() const;
  uint64_t packets_recv() const;
  std::string last_error() const;
  std::string stats_string() const;     // one-line quiche stats snapshot.

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace weblox
