// Host-side RakNet datagram seam (native C++ port of v2-wasmtime/src/raknet.rs).
//
// The patched guest has no sockets: RakNet's sendto copies a datagram into the
// TX ring in shared linear memory, and its select()/recvfrom() pair waits on
// and drains the RX ring. This module is the other end of those rings: one
// native UDP socket, one thread pushing the TX ring to it, one thread pulling
// replies into the RX ring. It knows nothing about RakNet packets beyond the
// three RUPP fields the join script owns (see Rupp).
//
// This header is deliberately independent of any VMContext layout: start()
// takes a raw pointer to guest linear memory and a wake callback for the
// guest's futex/atomic notify. See RAKNET.md for the ABI and lifecycle.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace weblox {
namespace raknet {

// Absolute offsets in the guest's 2 GiB shared memory (tools/patch.py ABI).
constexpr std::uint32_t kRingBase = 0x7FF00000u;
constexpr std::uint32_t kMagicValue = 0x4B52414Eu; // 'RAKN'
constexpr std::uint32_t kMagic = 0x00u;
constexpr std::uint32_t kTxHead = 0x04u;
constexpr std::uint32_t kTxTail = 0x08u;
constexpr std::uint32_t kRxHead = 0x0Cu;
constexpr std::uint32_t kRxTail = 0x10u;
constexpr std::uint32_t kWake = 0x14u;
constexpr std::uint32_t kSelects = 0x18u;
constexpr std::uint32_t kRecvd = 0x1Cu;
constexpr std::uint32_t kAddr = 0x20u;
constexpr std::uint32_t kPeer = 0x30u;
constexpr std::uint32_t kUpdates = 0x34u;
constexpr std::uint32_t kCreq = 0x38u;
constexpr std::uint32_t kCres = 0x3Cu;
constexpr std::uint32_t kHost = 0x40u;
constexpr std::size_t kHostLen = 48u;
constexpr std::uint32_t kPort = 0x70u;
constexpr std::uint32_t kTx0 = 0x80u;
constexpr std::uint32_t kSlot = 1536u;  // tools/patch.py / iwa/abi.js
constexpr std::uint32_t kNSlot = 64u;   // power of two; index is masked
constexpr std::size_t kMaxDatagram = 1500u;
constexpr std::uint32_t kRx0 = kTx0 + kNSlot * kSlot;
constexpr std::size_t kMemorySize = 0x80000000ull; // 32768 wasm pages

/// The fields of a join script the host consumes.
struct JoinTarget {
  std::string host;                                  // UdmuxEndpoints[0].Address
  std::uint16_t port = 0;                            // UdmuxEndpoints[0].Port
  std::optional<std::array<std::uint8_t, 4>> machine;     // MachineAddress
  std::optional<std::uint16_t> netstack_port;             // NetStackPort
  std::optional<std::array<std::uint8_t, 16>> token;      // NetStackTokenValue
  std::optional<std::uint16_t> data_port;                 // ServerConnections[0].Port
};

/// Parse a join script object, or a JSON string containing one.
/// Throws std::runtime_error when the peer endpoint is missing or malformed.
JoinTarget parseJoin(const std::string& script);

/// The three join-script-owned fields of RUPP, ported from iwa/rupp.js.
/// The join and session tokens are 16 bytes; RCC is (IPv4, port).
class Rupp {
 public:
  Rupp() = default;

  /// Take the values a join script names. Best effort: returns silently on
  /// malformed JSON, and never overwrites values already learned.
  Rupp& setJoinScript(const std::string& script);

  /// Rewrite the fields this session owns, in place, inside one datagram.
  void stamp(std::uint8_t* data, std::size_t len);
  void stamp(std::vector<std::uint8_t>& dgram) { stamp(dgram.data(), dgram.size()); }

  /// Remember the session token the server issues in Reply2.
  void learn(const std::uint8_t* data, std::size_t len);
  void learn(const std::vector<std::uint8_t>& dgram) { learn(dgram.data(), dgram.size()); }

  const std::optional<std::array<std::uint8_t, 16>>& join() const { return join_; }
  const std::optional<std::array<std::uint8_t, 16>>& session() const { return session_; }
  const std::optional<std::pair<std::array<std::uint8_t, 4>, std::uint16_t>>& rcc() const {
    return rcc_;
  }
  std::uint16_t data_port() const { return data_port_; }

 private:
  std::optional<std::array<std::uint8_t, 16>> join_;
  std::optional<std::array<std::uint8_t, 16>> session_;
  std::optional<std::pair<std::array<std::uint8_t, 4>, std::uint16_t>> rcc_;
  std::uint16_t data_port_ = 0;
};

struct Stats {
  std::uint64_t tx = 0;
  std::uint64_t rx = 0;
  std::uint64_t dropped = 0;
  std::uint64_t errors = 0;
};

/// Snapshot of pump counters (safe to call before start()).
Stats stats();

/// Request that both pumps exit at their next poll/recv timeout. Nonblocking.
void stop();

/// stop() plus join of both pump threads. Call from a non-pump thread after
/// the guest has stopped reading/writing the ring. Allows a later start().
void shutdown();

struct StartOptions {
  std::size_t memory_size = kMemorySize; // bounds for guest-memory accesses
  std::uint32_t slot = 0;                // 0 => WC_RK_SLOT env or kSlot
  bool arm_connect = true;               // set CREQ=1 once the pumps are live
};

/// Bind one native UDP socket and connect the guest's rings to it.
///
/// The socket is bound to an ephemeral local port and kept for the whole
/// session. On failure a std::runtime_error/std::system_error is thrown and
/// the started flag is released, so start() may be retried.
///
/// `guest_memory_base` points at linear-memory address 0; `wake` is invoked
/// from the RX thread after a datagram is published, and should perform the
/// guest notify on RING_BASE + kWake (the integrator owns the VMContext).
/// Empty host/port are filled in from the join script.
void start(const std::string& host, std::uint16_t port, const std::string& script,
           std::uint8_t* guest_memory_base, std::function<void()> wake = {},
           const StartOptions& options = {});

} // namespace raknet
} // namespace weblox

// Convenience aliases for host code that does not use the weblox namespace.
namespace raknet = weblox::raknet;
namespace rk = weblox::raknet;
