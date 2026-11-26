// Native join-route helper: gamejoin POST + browser WebTransport route.
//
// The native host needs the same route the patched Web Client derives from a
// join script (see docs/WEB-PLAY-GATE.md and host/ROUTE.md). This module owns
// only the HTTP exchange and the derivation; it has no VMContext or transport
// dependencies. The raw joinScript is kept for the later RNA join.
#pragma once

#include <cstdint>
#include <string>

namespace weblox {

/// Bounds for fetch(). The defaults are generous; the standalone probe uses a
/// shorter per-attempt timeout so one invocation stays well under a minute.
struct FetchOptions {
  unsigned max_attempts = 5;      // Roblox intermittently returns status 0 / empty token
  long timeout_ms = 30000;        // total time per HTTP attempt
  long connect_timeout_ms = 10000;
  unsigned retry_delay_ms = 250;  // linear backoff between attempts
};

/// The derived browser WebTransport route for one game join.
struct JoinRoute {
  std::string fqdn;      // <token>-<machine>-<port>.<browser-ip>.qdmux.roblox.com
  std::string host;      // browser/qdmux VIP: UdmuxEndpoints[0].Address, last octet + 1
  std::uint16_t port = 0; // NetStackPort (the port the WT handshake uses)
  std::string caps;      // NetStackConfig.caps (x-rbx-capabilities)
  std::string cert_hash; // NetStackConfig.certHashes[0], base64 SHA-256 pin
  std::string pub_key;   // NetStackConfig.pubKey, may be empty

  // Context carried forward for the RNA join/transport setup.
  std::string join_script;      // decoded joinScript as JSON text
  std::string netstack_config;  // URL-decoded NetStackConfig as JSON text
  std::string native_host;      // UdmuxEndpoints[0].Address (the native .33 VIP)
  std::uint16_t native_port = 0;
  std::uint64_t place_id = 0;
  std::string game_id;          // reply.jobId
  long status = -1;             // reply.status (2 on an approved join)

  /// Same as weblox::fetch(placeId, cookie) below.
  static JoinRoute fetch(std::uint64_t placeId, const std::string& cookie);
};

/// POST https://gamejoin.roblox.com/v1/join-game with the native identity,
/// retry the bounded number of times if Roblox answers status 0 or with an
/// unusable NetStackTokenValue, and derive the browser qdmux route.
///
/// Throws std::runtime_error with the reason on transport, HTTP, JSON or
/// derivation failure. No route is returned from an expired/incomplete reply.
JoinRoute fetch(std::uint64_t placeId, const std::string& cookie);
JoinRoute fetch(std::uint64_t placeId, const std::string& cookie,
                const FetchOptions& options);

} // namespace weblox
