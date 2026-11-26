// Implementation of weblox::WebTransport. See wt.h and WT.md.
#include "wt.h"

#include <quiche.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

// Supplied by host/build_quiche.sh: quiche 0.30.1's Rust H3 config has this
// method but its C header does not. The patched header declares it too; the
// declaration here keeps wt.cpp usable with an unpatched header as long as the
// linked archive contains the wrapper.
extern "C" int quiche_h3_config_set_additional_settings(
    quiche_h3_config* config, const uint64_t* keys, const uint64_t* values,
    size_t len);

namespace weblox {
namespace {

constexpr size_t kRecvBufSize = 65'535;
constexpr size_t kStreamBufSize = 16'384;

std::string quiche_error(int rc) {
  switch (rc) {
    case QUICHE_ERR_DONE: return "Done";
    case QUICHE_ERR_BUFFER_TOO_SHORT: return "BufferTooShort";
    case QUICHE_ERR_UNKNOWN_VERSION: return "UnknownVersion";
    case QUICHE_ERR_INVALID_FRAME: return "InvalidFrame";
    case QUICHE_ERR_INVALID_PACKET: return "InvalidPacket";
    case QUICHE_ERR_INVALID_STATE: return "InvalidState";
    case QUICHE_ERR_INVALID_STREAM_STATE: return "InvalidStreamState";
    case QUICHE_ERR_INVALID_TRANSPORT_PARAM: return "InvalidTransportParam";
    case QUICHE_ERR_CRYPTO_FAIL: return "CryptoFail";
    case QUICHE_ERR_TLS_FAIL: return "TlsFail";
    case QUICHE_ERR_FLOW_CONTROL: return "FlowControl";
    case QUICHE_ERR_STREAM_LIMIT: return "StreamLimit";
    case QUICHE_ERR_FINAL_SIZE: return "FinalSize";
    case QUICHE_ERR_CONGESTION_CONTROL: return "CongestionControl";
    default: return "quiche error " + std::to_string(rc);
  }
}

std::string bytes_to_string(const uint8_t* p, size_t n) {
  return std::string(reinterpret_cast<const char*>(p), n);
}

std::string hex_string(const uint8_t* p, size_t n) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kHex[p[i] >> 4]);
    out.push_back(kHex[p[i] & 15]);
  }
  return out;
}

// --- SHA-256 (FIPS 180-4), used to pin the peer certificate DER hash. -------
namespace sha256 {

struct Ctx {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block[64] = {};
  uint64_t total = 0;
};

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32 - n));
}

void compress(Ctx& c, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
           (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
  uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + ch + kK[i] + w[i];
    uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = s0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
  c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

void update(Ctx& c, const uint8_t* p, size_t n) {
  size_t fill = c.total & 63;
  c.total += n;
  while (n) {
    size_t take = std::min(n, size_t(64) - fill);
    std::memcpy(c.block + fill, p, take);
    fill += take;
    if (fill == 64) {
      compress(c, c.block);
      fill = 0;
    }
    p += take;
    n -= take;
  }
}

std::array<uint8_t, 32> finish(Ctx& c) {
  uint64_t bits = c.total * 8;
  uint8_t pad = 0x80;
  update(c, &pad, 1);
  uint8_t zero = 0;
  while ((c.total & 63) != 56) update(c, &zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
  update(c, len, 8);
  std::array<uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i) {
    out[4 * i] = uint8_t(c.h[i] >> 24);
    out[4 * i + 1] = uint8_t(c.h[i] >> 16);
    out[4 * i + 2] = uint8_t(c.h[i] >> 8);
    out[4 * i + 3] = uint8_t(c.h[i]);
  }
  return out;
}

std::array<uint8_t, 32> hash(const uint8_t* p, size_t n) {
  Ctx c;
  update(c, p, n);
  return finish(c);
}

}  // namespace sha256

std::string base64_encode(const uint8_t* p, size_t n) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(p[i]) << 16;
    if (i + 1 < n) v |= uint32_t(p[i + 1]) << 8;
    if (i + 2 < n) v |= uint32_t(p[i + 2]);
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < n ? kAlphabet[v & 63] : '=');
  }
  return out;
}

bool base64_decode(const std::string& in, std::vector<uint8_t>* out) {
  int val[256];
  std::memset(val, -1, sizeof(val));
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (int i = 0; i < 64; ++i) val[uint8_t(kAlphabet[i])] = i;
  uint32_t acc = 0;
  int bits = 0;
  out->clear();
  for (char ch : in) {
    if (ch == '=') break;
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
    int v = val[uint8_t(ch)];
    if (v < 0) return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(uint8_t(acc >> bits));
    }
  }
  return true;
}

// QUIC varint (RFC 9000 section 16).
size_t encode_varint(uint64_t v, uint8_t* out) {
  if (v < 64) {
    out[0] = uint8_t(v);
    return 1;
  }
  if (v < 16384) {
    out[0] = uint8_t(0x40 | (v >> 8));
    out[1] = uint8_t(v);
    return 2;
  }
  if (v < (uint64_t(1) << 30)) {
    out[0] = uint8_t(0x80 | (v >> 24));
    out[1] = uint8_t(v >> 16);
    out[2] = uint8_t(v >> 8);
    out[3] = uint8_t(v);
    return 4;
  }
  out[0] = uint8_t(0xc0 | (v >> 56));
  out[1] = uint8_t(v >> 48);
  out[2] = uint8_t(v >> 40);
  out[3] = uint8_t(v >> 32);
  out[4] = uint8_t(v >> 24);
  out[5] = uint8_t(v >> 16);
  out[6] = uint8_t(v >> 8);
  out[7] = uint8_t(v);
  return 8;
}

bool decode_varint(const uint8_t* p, size_t n, uint64_t* v, size_t* used) {
  if (n == 0) return false;
  unsigned width = 1 << (p[0] >> 6);
  if (width > n) return false;
  uint64_t value = p[0] & 0x3f;
  for (unsigned i = 1; i < width; ++i)
    value = (value << 8) | uint64_t(p[i]);
  *v = value;
  *used = width;
  return true;
}

bool parse_address(const std::string& host, uint16_t port,
                   sockaddr_storage* out, socklen_t* len) {
  std::memset(out, 0, sizeof(*out));
  if (inet_pton(AF_INET, host.c_str(), &reinterpret_cast<sockaddr_in*>(out)->sin_addr) == 1) {
    auto* a = reinterpret_cast<sockaddr_in*>(out);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    *len = sizeof(sockaddr_in);
    return true;
  }
  if (inet_pton(AF_INET6, host.c_str(),
                &reinterpret_cast<sockaddr_in6*>(out)->sin6_addr) == 1) {
    auto* a = reinterpret_cast<sockaddr_in6*>(out);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    *len = sizeof(sockaddr_in6);
    return true;
  }
  return false;
}

std::string connection_errors(const quiche_conn* conn) {
  std::ostringstream os;
  bool is_app = false;
  uint64_t code = 0;
  const uint8_t* reason = nullptr;
  size_t reason_len = 0;
  if (quiche_conn_local_error(conn, &is_app, &code, &reason, &reason_len)) {
    os << "local=" << code << ":"
       << bytes_to_string(reason, reason_len);
  }
  if (quiche_conn_peer_error(conn, &is_app, &code, &reason, &reason_len)) {
    if (os.tellp() != std::ostream::pos_type(0)) os << " ";
    os << "peer=" << code << ":"
       << bytes_to_string(reason, reason_len);
  }
  if (os.tellp() == std::ostream::pos_type(0)) os << "none";
  return os.str();
}

int h3_header_callback(uint8_t* name, size_t name_len, uint8_t* value,
                       size_t value_len, void* arg) {
  auto* headers =
      static_cast<std::vector<std::pair<std::string, std::string>>*>(arg);
  headers->emplace_back(bytes_to_string(name, name_len),
                        bytes_to_string(value, value_len));
  return 0;
}

}  // namespace

// Per-stream WT_UNI/WT_BI prefix parsing state (external linkage so Impl,
// which is a member of WebTransport, can hold it without a linkage warning).
struct StreamState {
  bool prefix_parsed = false;
  bool ignore = false;
  bool unidirectional = false;
  uint64_t session_id = 0;
  std::vector<uint8_t> bytes;
};

struct WebTransport::Impl {
  Options options;
  std::string host;
  uint16_t port = 0;
  std::string fqdn;
  std::string caps;
  std::string cert_hash;
  std::vector<uint8_t> expected_cert_hash;
  std::string authority;

  int sockfd = -1;
  sockaddr_storage local{};
  socklen_t local_len = 0;
  sockaddr_storage peer{};
  socklen_t peer_len = 0;

  quiche_config* config = nullptr;
  quiche_conn* conn = nullptr;
  quiche_h3_config* h3_config = nullptr;
  quiche_h3_conn* h3 = nullptr;

  std::vector<uint8_t> out_buf;
  std::vector<uint8_t> recv_buf;
  Status status = Status::kIdle;
  int status_code = 0;
  uint64_t session_id = kNoSession;
  bool connected = false;
  bool cert_checked = false;
  bool cert_verified = false;
  bool cert_pin_warned = false;
  bool h3_created = false;
  bool request_sent = false;
  bool settings_logged = false;
  std::string last_error;

  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point next_keepalive;
  uint64_t keepalives = 0;
  uint64_t packets_sent = 0;
  uint64_t packets_recv = 0;
  uint64_t on_timeout_calls = 0;
  uint64_t errors = 0;

  std::deque<std::vector<uint8_t>> datagrams;
  std::map<uint64_t, StreamState> streams;
  std::deque<StreamMessage> stream_messages;
  uint64_t next_bi_stream = 4;    // 0 is the H3 CONNECT request stream.
  uint64_t next_uni_stream = 14;  // 2/6/10 belong to H3 control/QPACK.

  LogFn log_fn;
  StatusFn status_fn;
  DatagramFn datagram_fn;
  StreamFn stream_fn;

  void log(const std::string& line) {
    if (log_fn) log_fn(line);
  }

  void set_status(Status s, const std::string& message) {
    status = s;
    if (status_fn) status_fn(s, message);
  }

  void fail(const std::string& message) {
    last_error = message;
    ++errors;
    log("[ERROR] " + message);
    set_status(Status::kError, message);
  }

  std::string elapsed() const {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return std::to_string(ms) + "ms";
  }

  bool init(const std::string& h, uint16_t p, const std::string& fqdn_in,
            const std::string& caps_in, const std::string& cert_hash_in,
            const Options& opts) {
    host = h;
    port = p;
    fqdn = fqdn_in;
    caps = caps_in;
    cert_hash = cert_hash_in;
    options = opts;
    authority = fqdn + ":" + std::to_string(port);
    started = std::chrono::steady_clock::now();
    out_buf.resize(options.max_datagram_size);
    recv_buf.resize(kRecvBufSize);
    if (!cert_hash.empty() && !base64_decode(cert_hash, &expected_cert_hash)) {
      fail("invalid base64 cert hash");
      return false;
    }
    if (expected_cert_hash.size() != 32 && !cert_hash.empty()) {
      fail("cert hash must decode to 32 bytes (SHA-256)");
      return false;
    }

    if (!parse_address(host, port, &peer, &peer_len)) {
      fail("cannot parse UDP peer address: " + host);
      return false;
    }
    sockfd = socket(peer.ss_family, SOCK_DGRAM, 0);
    if (sockfd < 0) {
      fail(std::string("socket: ") + strerror(errno));
      return false;
    }
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_storage bind_addr{};
    socklen_t bind_len = 0;
    if (peer.ss_family == AF_INET) {
      auto* a = reinterpret_cast<sockaddr_in*>(&bind_addr);
      a->sin_family = AF_INET;
      a->sin_addr.s_addr = INADDR_ANY;
      bind_len = sizeof(sockaddr_in);
    } else {
      auto* a = reinterpret_cast<sockaddr_in6*>(&bind_addr);
      a->sin6_family = AF_INET6;
      a->sin6_addr = in6addr_any;
      bind_len = sizeof(sockaddr_in6);
    }
    if (::bind(sockfd, reinterpret_cast<sockaddr*>(&bind_addr), bind_len) < 0) {
      fail(std::string("bind: ") + strerror(errno));
      return false;
    }
    socklen_t got = sizeof(local);
    if (getsockname(sockfd, reinterpret_cast<sockaddr*>(&local), &got) == 0)
      local_len = got;

    // Exact transport settings from the proven Rust probe.
    config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!config) {
      fail("quiche_config_new failed");
      return false;
    }
    const uint8_t alpn[] = {2, 'h', '3'};
    if (quiche_config_set_application_protos(config, alpn, sizeof(alpn)) != 0) {
      fail("set ALPN h3 failed");
      return false;
    }
    quiche_config_set_max_idle_timeout(config, options.idle_timeout_ms);
    quiche_config_set_max_recv_udp_payload_size(config, options.max_datagram_size);
    quiche_config_set_max_send_udp_payload_size(config, options.max_datagram_size);
    quiche_config_set_disable_active_migration(config, true);
    quiche_config_set_initial_max_data(config, 15'728'640);
    quiche_config_set_initial_max_stream_data_bidi_local(config, 6'291'456);
    quiche_config_set_initial_max_stream_data_bidi_remote(config, 6'291'456);
    quiche_config_set_initial_max_stream_data_uni(config, 6'291'456);
    quiche_config_set_initial_max_streams_bidi(config, 100);
    quiche_config_set_initial_max_streams_uni(config, 100);
    quiche_config_enable_dgram(config, true, 100, 100);
    // quiche's Config has no verify callback; BoringSSL CA validation is off.
    // We compensate with the route's SHA-256 DER hash pin below.
    quiche_config_verify_peer(config, false);

    h3_config = quiche_h3_config_new();
    if (!h3_config) {
      fail("quiche_h3_config_new failed");
      return false;
    }
    quiche_h3_config_set_max_field_section_size(h3_config, 65'536);
    quiche_h3_config_set_qpack_max_table_capacity(h3_config, 65'536);
    quiche_h3_config_set_qpack_blocked_streams(h3_config, 100);
    quiche_h3_config_enable_extended_connect(h3_config, true);
    const uint64_t keys[] = {0xffd277, 0xc671706a, 0x2b603743, 0x2b603742};
    const uint64_t values[] = {1, 1, 1, 1};
    if (quiche_h3_config_set_additional_settings(h3_config, keys, values, 4) != 0)
      fail("warning: H3 draft-02 settings were rejected by this quiche build");

    uint8_t scid[16];
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.read(reinterpret_cast<char*>(scid), sizeof(scid))) {
      fail("cannot read /dev/urandom for SCID");
      return false;
    }
    conn = quiche_connect(fqdn.c_str(), scid, sizeof(scid),
                          reinterpret_cast<sockaddr*>(&local), local_len,
                          reinterpret_cast<sockaddr*>(&peer), peer_len,
                          config);
    if (!conn) {
      fail("quiche_connect failed");
      return false;
    }

    log("[wt] t=" + elapsed() + " ALPN=h3 local_fd=" + std::to_string(sockfd) +
        " peer=" + host + ":" + std::to_string(port) + " sni=" + fqdn +
        " authority=" + authority);
    if (expected_cert_hash.empty() && options.require_cert_hash)
      log("[WARNING] no cert hash supplied: peer certificate is NOT pinned");
    log("[WARNING] quiche Config has no verify callback: BoringSSL CA/chain "
        "validation disabled; only the SHA-256 DER hash pin is checked.");

    size_t n = 0;
    quiche_send_info info{};
    ssize_t written = quiche_conn_send(conn, out_buf.data(), out_buf.size(), &info);
    if (written > 0) {
      n = size_t(written);
      ::sendto(sockfd, out_buf.data(), n, 0,
               reinterpret_cast<sockaddr*>(&peer), peer_len);
      ++packets_sent;
      log("[quic] t=" + elapsed() + " sent initial packet (" +
          std::to_string(n) + " bytes)");
    } else {
      fail(std::string("initial quiche_conn_send failed: ") +
           quiche_error(int(written)));
      return false;
    }
    next_keepalive = started + std::chrono::milliseconds(options.keepalive_ms);
    set_status(Status::kConnecting, "QUIC Initial sent");
    return true;
  }

  void flush() {
    if (!conn) return;
    while (true) {
      quiche_send_info info{};
      ssize_t n = quiche_conn_send(conn, out_buf.data(), out_buf.size(), &info);
      if (n == QUICHE_ERR_DONE) break;
      if (n < 0) {
        fail(std::string("quiche_conn_send: ") + quiche_error(int(n)));
        break;
      }
      ssize_t sent = ::sendto(sockfd, out_buf.data(), size_t(n), 0,
                              reinterpret_cast<const sockaddr*>(&info.to),
                              info.to_len);
      if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        fail(std::string("UDP sendto: ") + strerror(errno));
      if (sent > 0) ++packets_sent;
    }
  }

  void check_cert() {
    if (cert_checked) return;
    cert_checked = true;
    const uint8_t* der = nullptr;
    size_t der_len = 0;
    quiche_conn_peer_cert(conn, &der, &der_len);
    if (expected_cert_hash.empty()) {
      if (options.require_cert_hash && !cert_pin_warned) {
        cert_pin_warned = true;
        log("[WARNING] no pinned cert hash supplied for this route");
      }
      return;
    }
    if (!der || der_len == 0) {
      fail("no peer certificate in QUIC handshake");
      return;
    }
    auto digest = sha256::hash(der, der_len);
    std::string got = base64_encode(digest.data(), digest.size());
    if (got != cert_hash) {
      fail("certificate hash mismatch: pinned=" + cert_hash + " got=" + got +
           " (DER " + std::to_string(der_len) + " bytes, sha256 " +
           hex_string(digest.data(), digest.size()) + ")");
      return;
    }
    cert_verified = true;
    log("[cert] t=" + elapsed() +
        " SHA-256 DER hash matches pinned route hash (" + got + ")");
  }

  void create_h3() {
    if (h3_created) return;
    h3 = quiche_h3_conn_new_with_transport(conn, h3_config);
    if (!h3) {
      fail("quiche_h3_conn_new_with_transport failed");
      return;
    }
    h3_created = true;
    log("[h3] t=" + elapsed() + " local SETTINGS sent on control stream");
  }

  void log_peer_settings() {
    if (settings_logged) return;
    std::vector<std::pair<uint64_t, uint64_t>> settings;
    struct Cb {
      static int run(uint64_t id, uint64_t value, void* arg) {
        static_cast<std::vector<std::pair<uint64_t, uint64_t>>*>(arg)
            ->emplace_back(id, value);
        return 0;
      }
    };
    if (quiche_h3_for_each_setting(h3, Cb::run, &settings) < 0 || settings.empty())
      return;  // peer SETTINGS have not arrived yet; retry next iteration.
    settings_logged = true;
    std::ostringstream os;
    for (size_t i = 0; i < settings.size(); ++i) {
      if (i) os << ", ";
      os << settings[i].first << "=" << settings[i].second;
    }
    log("[h3] t=" + elapsed() + " peer SETTINGS: [" + os.str() + "]");
  }

  void send_connect() {
    if (request_sent) return;
    std::string names[] = {":method",           ":scheme",
                           ":authority",        ":path",
                           ":protocol",         "sec-webtransport-http3-draft02",
                           "x-rbx-capabilities"};
    std::string values[] = {"CONNECT", "https", authority, "/wt",
                            "webtransport", "1", caps};
    std::array<quiche_h3_header, 7> headers{};
    for (size_t i = 0; i < 7; ++i) {
      headers[i].name = reinterpret_cast<const uint8_t*>(names[i].data());
      headers[i].name_len = names[i].size();
      headers[i].value = reinterpret_cast<const uint8_t*>(values[i].data());
      headers[i].value_len = values[i].size();
    }
    int64_t stream = quiche_h3_send_request(h3, conn, headers.data(),
                                            headers.size(), false);
    if (stream < 0) {
      // -13 is H3 StreamBlocked; retry on the next pump iteration.
      if (stream != -13)
        fail(std::string("quiche_h3_send_request: ") + quiche_error(int(stream)));
      return;
    }
    request_sent = true;
    session_id = uint64_t(stream);
    log("[h3] t=" + elapsed() + " extended CONNECT sent on stream " +
        std::to_string(stream) + " (" + authority + "/wt, protocol=webtransport, "
        "draft02=1, caps=" + caps + ")");
  }

  void poll_h3() {
    if (!h3) return;
    while (true) {
      quiche_h3_event* ev = nullptr;
      int64_t stream = quiche_h3_conn_poll(h3, conn, &ev);
      if (ev) {
        if (quiche_h3_event_type(ev) == QUICHE_H3_EVENT_HEADERS) {
          std::vector<std::pair<std::string, std::string>> fields;
          quiche_h3_event_for_each_header(ev, h3_header_callback, &fields);
          std::ostringstream os;
          for (size_t i = 0; i < fields.size(); ++i) {
            if (i) os << ", ";
            os << fields[i].first << ": " << fields[i].second;
          }
          log("[h3] t=" + elapsed() + " response stream=" +
              std::to_string(stream) + " headers=[" + os.str() + "]");
          if (status_code == 0) {
            for (const auto& f : fields) {
              if (f.first == ":status") {
                status_code = std::atoi(f.second.c_str());
                break;
              }
            }
            if (status_code == 200 && !connected) {
              connected = true;
              next_keepalive = std::chrono::steady_clock::now();
              log("[RESULT] t=" + elapsed() + " CONNECT :status = 200");
              set_status(Status::kConnected, "CONNECT 200; session id " +
                                                std::to_string(session_id));
            } else if (status_code != 0 && status_code != 200) {
              fail("extended CONNECT rejected: :status " +
                   std::to_string(status_code));
            }
          }
        }
        quiche_h3_event_free(ev);
      }
      if (stream == QUICHE_ERR_DONE) break;
      if (stream < 0) {
        fail(std::string("quiche_h3_conn_poll: ") + quiche_error(int(stream)));
        break;
      }
    }
  }

  void drain_datagrams() {
    if (!connected) return;
    while (true) {
      ssize_t front = quiche_conn_dgram_recv_front_len(conn);
      if (front <= 0) break;
      std::vector<uint8_t> buf(kRecvBufSize);
      ssize_t n = quiche_conn_dgram_recv(conn, buf.data(), buf.size());
      if (n <= 0) break;
      uint64_t sid = 0;
      size_t used = 0;
      if (!decode_varint(buf.data(), size_t(n), &sid, &used) || sid != session_id) {
        log("[datagram] ignoring datagram with session id " +
            std::to_string(sid) + " (expected " +
            std::to_string(session_id) + ")");
        continue;
      }
      std::vector<uint8_t> payload(buf.begin() + used, buf.begin() + n);
      if (datagram_fn) {
        datagram_fn(payload);
      } else {
        datagrams.push_back(std::move(payload));
      }
    }
  }

  bool is_h3_internal_stream(uint64_t sid) const {
    // H3 client: bidi 0 = CONNECT, uni 2/6/10 = control/QPACK;
    // H3 server: uni 3/7/11 = control/QPACK. Never consume their bytes here.
    return sid == session_id || sid == 2 || sid == 6 || sid == 10 ||
           sid == 3 || sid == 7 || sid == 11;
  }

  void handle_stream_bytes(uint64_t sid, StreamState& st) {
    if (st.ignore || st.bytes.empty()) return;
    if (!st.prefix_parsed) {
      if (st.bytes.size() < 1) return;
      uint8_t type = st.bytes[0];
      if (type != 0x54 && type != 0x41) {
        log("[stream] " + std::to_string(sid) +
            " is not a WebTransport stream (type byte " +
            std::to_string(type) + "); ignoring");
        st.ignore = true;
        st.bytes.clear();
        return;
      }
      st.unidirectional = (type == 0x54);
      uint64_t sid_from_header = 0;
      size_t used = 0;
      if (!decode_varint(st.bytes.data() + 1, st.bytes.size() - 1,
                         &sid_from_header, &used)) {
        return;  // wait for more bytes of the session-id varint.
      }
      if (sid_from_header != session_id) {
        log("[stream] " + std::to_string(sid) + " belongs to session " +
            std::to_string(sid_from_header) + ", not " +
            std::to_string(session_id) + "; ignoring");
        st.ignore = true;
        st.bytes.clear();
        return;
      }
      st.bytes.erase(st.bytes.begin(), st.bytes.begin() + 1 + used);
      st.prefix_parsed = true;
      log("[stream] t=" + elapsed() + " accepted server-initiated " +
          (st.unidirectional ? "WT_UNI" : "WT_BI") + " stream " +
          std::to_string(sid) + " (session " + std::to_string(session_id) + ")");
    }
    if (st.bytes.empty()) return;
    StreamMessage msg;
    msg.stream_id = sid;
    msg.unidirectional = st.unidirectional;
    msg.data = std::move(st.bytes);
    st.bytes.clear();
    if (stream_fn) {
      stream_fn(msg);
    } else {
      stream_messages.push_back(std::move(msg));
    }
  }

  void drain_streams() {
    if (!connected) return;
    quiche_stream_iter* iter = quiche_conn_readable(conn);
    if (!iter) return;
    uint64_t sid = 0;
    std::vector<uint64_t> ids;
    while (quiche_stream_iter_next(iter, &sid)) ids.push_back(sid);
    quiche_stream_iter_free(iter);
    for (uint64_t stream : ids) {
      if (is_h3_internal_stream(stream)) continue;
      StreamState& st = streams[stream];
      if (st.ignore) continue;
      bool fin = false;
      uint64_t err = 0;
      uint8_t buf[kStreamBufSize];
      while (true) {
        ssize_t n = quiche_conn_stream_recv(conn, stream, buf, sizeof(buf),
                                            &fin, &err);
        if (n == QUICHE_ERR_DONE) break;
        if (n < 0) {
          log("[stream] t=" + elapsed() + " stream " + std::to_string(stream) +
              " recv error " + quiche_error(int(n)));
          break;
        }
        st.bytes.insert(st.bytes.end(), buf, buf + n);
        handle_stream_bytes(stream, st);
        if (size_t(n) < sizeof(buf)) break;
      }
    }
  }

  bool pump_once() {
    if (!conn || status == Status::kError) return false;
    if (quiche_conn_is_closed(conn)) {
      log("[CLOSE] t=" + elapsed() + " conn closed: " + connection_errors(conn));
      set_status(Status::kClosed, connection_errors(conn));
      return false;
    }

    auto now = std::chrono::steady_clock::now();
    if (connected && now >= next_keepalive) {
      ssize_t rc = quiche_conn_send_ack_eliciting(conn);
      if (rc == QUICHE_ERR_DONE || rc >= 0) {
        ++keepalives;
        log("[keepalive] t=" + elapsed() + " QUIC PING #" +
            std::to_string(keepalives) + " (cadence=" +
            std::to_string(options.keepalive_ms) + "ms)");
      } else {
        ++errors;
        log("[ERROR] t=" + elapsed() + " send_ack_eliciting: " +
            quiche_error(int(rc)));
      }
      next_keepalive = now + std::chrono::milliseconds(options.keepalive_ms);
    }
    flush();

    // Sleep until the earliest of quiche's timer, the keepalive deadline and
    // 100 ms (as in the longevity probe, so the PING cadence stays precise).
    int wait_ms = 100;
    uint64_t quiche_timeout = quiche_conn_timeout_as_millis(conn);
    if (quiche_timeout != UINT64_MAX)
      wait_ms = int(std::min<uint64_t>(wait_ms, std::max<uint64_t>(1, quiche_timeout)));
    if (connected) {
      int64_t until_ping = std::chrono::duration_cast<std::chrono::milliseconds>(
          next_keepalive - now).count();
      if (until_ping >= 0) wait_ms = std::min(wait_ms, int(std::min<int64_t>(100, until_ping)));
    }
    pollfd pfd{};
    pfd.fd = sockfd;
    pfd.events = POLLIN;
    int ready = ::poll(&pfd, 1, wait_ms);
    if (ready > 0 && (pfd.revents & POLLIN)) {
      sockaddr_storage from{};
      socklen_t from_len = sizeof(from);
      ssize_t n = ::recvfrom(sockfd, recv_buf.data(), recv_buf.size(), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n > 0) {
        quiche_recv_info info{};
        info.from = reinterpret_cast<sockaddr*>(&from);
        info.from_len = from_len;
        info.to = reinterpret_cast<sockaddr*>(&local);
        info.to_len = local_len;
        ssize_t processed = quiche_conn_recv(conn, recv_buf.data(), size_t(n), &info);
        ++packets_recv;
        if (processed < 0 && processed != QUICHE_ERR_DONE) {
          ++errors;
          log("[ERROR] t=" + elapsed() + " quiche_conn_recv: " +
              quiche_error(int(processed)));
        }
      }
    } else if (ready == 0) {
      ++on_timeout_calls;
      quiche_conn_on_timeout(conn);
    } else if (errno != EINTR) {
      ++errors;
      log("[ERROR] t=" + elapsed() + " poll: " + std::string(strerror(errno)));
      quiche_conn_on_timeout(conn);
    }

    if (quiche_conn_is_closed(conn)) {
      log("[CLOSE] t=" + elapsed() + " conn closed: " + connection_errors(conn));
      set_status(Status::kClosed, connection_errors(conn));
      return false;
    }
    if (quiche_conn_is_established(conn)) {
      check_cert();
      if (status == Status::kError) return false;
      create_h3();
      if (status == Status::kError) return false;
      if (h3) {
        log_peer_settings();
        send_connect();
        if (status == Status::kError) return false;
        poll_h3();
        if (status == Status::kError) return false;
      }
    }
    drain_datagrams();
    drain_streams();
    return true;
  }

  void teardown() {
    if (h3) {
      quiche_h3_conn_free(h3);
      h3 = nullptr;
    }
    if (h3_config) {
      quiche_h3_config_free(h3_config);
      h3_config = nullptr;
    }
    if (conn) {
      quiche_conn_free(conn);
      conn = nullptr;
    }
    if (config) {
      quiche_config_free(config);
      config = nullptr;
    }
    if (sockfd >= 0) {
      ::close(sockfd);
      sockfd = -1;
    }
  }
};

WebTransport::WebTransport() : impl_(std::make_unique<Impl>()) {}
WebTransport::~WebTransport() { close(); }

bool WebTransport::connect(const std::string& host, uint16_t port,
                           const std::string& fqdn, const std::string& caps,
                           const std::string& cert_hash) {
  return connect(host, port, fqdn, caps, cert_hash, Options());
}

bool WebTransport::connect(const std::string& host, uint16_t port,
                           const std::string& fqdn, const std::string& caps,
                           const std::string& cert_hash,
                           const Options& options) {
  close();
  if (!impl_->init(host, port, fqdn, caps, cert_hash, options)) return false;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(options.connect_timeout_ms);
  while (impl_->status_code != 200) {
    if (std::chrono::steady_clock::now() >= deadline) {
      impl_->fail("CONNECT timed out after " +
                  std::to_string(options.connect_timeout_ms) + "ms");
      return false;
    }
    if (!impl_->pump_once()) return false;
  }
  return impl_->connected && impl_->status != Status::kError;
}

void WebTransport::set_log_callback(LogFn fn) { impl_->log_fn = std::move(fn); }
void WebTransport::set_status_callback(StatusFn fn) { impl_->status_fn = std::move(fn); }
void WebTransport::set_datagram_callback(DatagramFn fn) { impl_->datagram_fn = std::move(fn); }
void WebTransport::set_stream_callback(StreamFn fn) { impl_->stream_fn = std::move(fn); }

bool WebTransport::run_for(std::chrono::milliseconds duration) {
  auto end = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < end) {
    if (!impl_->pump_once()) return false;
  }
  return is_open();
}

bool WebTransport::pump() { return impl_->pump_once(); }

bool WebTransport::send_datagram(std::vector<uint8_t> payload) {
  if (!is_open()) return false;
  uint8_t prefix[8];
  size_t used = encode_varint(session_id(), prefix);
  std::vector<uint8_t> frame(prefix, prefix + used);
  frame.insert(frame.end(), payload.begin(), payload.end());
  ssize_t max_len = quiche_conn_dgram_max_writable_len(impl_->conn);
  if (max_len >= 0 && frame.size() > size_t(max_len)) {
    impl_->fail("datagram " + std::to_string(frame.size()) +
                " bytes exceeds writable len " + std::to_string(max_len));
    return false;
  }
  ssize_t n = quiche_conn_dgram_send(impl_->conn, frame.data(), frame.size());
  if (n < 0) {
    impl_->fail(std::string("quiche_conn_dgram_send: ") + quiche_error(int(n)));
    return false;
  }
  impl_->flush();
  return true;
}

bool WebTransport::recv_datagram(std::vector<uint8_t>* payload) {
  if (!payload || impl_->datagrams.empty()) return false;
  *payload = std::move(impl_->datagrams.front());
  impl_->datagrams.pop_front();
  return true;
}

int64_t WebTransport::open_uni_stream() {
  if (!is_open() || quiche_conn_peer_streams_left_uni(impl_->conn) == 0) return -1;
  uint64_t sid = impl_->next_uni_stream;
  uint8_t header[9];
  header[0] = 0x54;  // WT_UNI_STREAM_TYPE
  size_t used = 1 + encode_varint(session_id(), header + 1);
  uint64_t err = 0;
  ssize_t n = quiche_conn_stream_send(impl_->conn, sid, header, used, false, &err);
  if (n != ssize_t(used)) return -1;
  impl_->next_uni_stream += 4;
  impl_->flush();
  return int64_t(sid);
}

int64_t WebTransport::open_bi_stream() {
  if (!is_open() || quiche_conn_peer_streams_left_bidi(impl_->conn) == 0) return -1;
  uint64_t sid = impl_->next_bi_stream;
  uint8_t header[9];
  header[0] = 0x41;  // WT_BI_STREAM_TYPE
  size_t used = 1 + encode_varint(session_id(), header + 1);
  uint64_t err = 0;
  ssize_t n = quiche_conn_stream_send(impl_->conn, sid, header, used, false, &err);
  if (n != ssize_t(used)) return -1;
  impl_->next_bi_stream += 4;
  impl_->flush();
  return int64_t(sid);
}

bool WebTransport::send_stream_data(uint64_t stream_id,
                                    const std::vector<uint8_t>& data) {
  if (!is_open() || data.empty()) return false;
  uint64_t err = 0;
  ssize_t n = quiche_conn_stream_send(impl_->conn, stream_id, data.data(),
                                      data.size(), false, &err);
  if (n < 0) return false;
  impl_->flush();
  return size_t(n) == data.size();
}

size_t WebTransport::drain_streams(std::vector<StreamMessage>* out) {
  size_t n = 0;
  while (!impl_->stream_messages.empty()) {
    if (out) out->push_back(std::move(impl_->stream_messages.front()));
    impl_->stream_messages.pop_front();
    ++n;
  }
  return n;
}

void WebTransport::close() {
  if (impl_->conn && !quiche_conn_is_closed(impl_->conn)) {
    const char reason[] = "weblox shutdown";
    quiche_conn_close(impl_->conn, true, 0,
                      reinterpret_cast<const uint8_t*>(reason), sizeof(reason) - 1);
    impl_->flush();
  }
  if (impl_->status != Status::kIdle && impl_->status != Status::kClosed &&
      impl_->status != Status::kError) {
    impl_->set_status(Status::kClosed, "closed by host");
  }
  impl_->teardown();
}

bool WebTransport::is_open() const {
  return impl_->conn && impl_->connected && !quiche_conn_is_closed(impl_->conn) &&
         impl_->status != Status::kError;
}

uint64_t WebTransport::session_id() const { return impl_->session_id; }
int WebTransport::status_code() const { return impl_->status_code; }
bool WebTransport::cert_pin_verified() const { return impl_->cert_verified; }
uint64_t WebTransport::keepalives_sent() const { return impl_->keepalives; }
uint64_t WebTransport::packets_sent() const { return impl_->packets_sent; }
uint64_t WebTransport::packets_recv() const { return impl_->packets_recv; }
std::string WebTransport::last_error() const { return impl_->last_error; }

std::string WebTransport::stats_string() const {
  if (!impl_->conn) return "no connection";
  quiche_stats s{};
  quiche_conn_stats(impl_->conn, &s);
  std::ostringstream os;
  os << "recv=" << s.recv << " sent=" << s.sent << " lost=" << s.lost
     << " retrans=" << s.retrans << " sent_bytes=" << s.sent_bytes
     << " recv_bytes=" << s.recv_bytes << " dgram_recv=" << s.dgram_recv
     << " dgram_sent=" << s.dgram_sent;
  return os.str();
}

}  // namespace weblox

#ifdef WEBLOX_WT_SELFTEST
// Bounded self-test: connect with a freshly generated route, then survive for
// --observe-secs (default 60). Compile with -DWEBLOX_WT_SELFTEST. No browser,
// Deno or game client is involved; only UDP + libquiche.
#include <cstdlib>

static void usage() {
  std::fprintf(stderr,
               "usage: wt-selftest --host IP --port N --fqdn FQDN --caps HEX "
               "[--cert-hash B64] [--observe-secs N] [--timeout-ms N]\n");
}

int main(int argc, char** argv) {
  std::string host, fqdn, caps, cert_hash;
  uint16_t port = 0;
  int observe = 60;
  uint64_t timeout_ms = 15'000;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      return i + 1 < argc ? std::string(argv[++i]) : std::string();
    };
    if (a == "--host") host = next();
    else if (a == "--port") port = uint16_t(std::atoi(next().c_str()));
    else if (a == "--fqdn") fqdn = next();
    else if (a == "--caps") caps = next();
    else if (a == "--cert-hash") cert_hash = next();
    else if (a == "--observe-secs") observe = std::atoi(next().c_str());
    else if (a == "--timeout-ms") timeout_ms = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--help") { usage(); return 0; }
    else { usage(); return 1; }
  }
  if (host.empty() || port == 0 || fqdn.empty()) { usage(); return 1; }

  auto start = std::chrono::steady_clock::now();
  auto elapsed = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count();
  };
  weblox::WebTransport wt;
  wt.set_log_callback([&](const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  });
  wt.set_status_callback([&](weblox::Status s, const std::string& msg) {
    std::printf("[status] t=%lldms status=%d %s\n", static_cast<long long>(elapsed()),
                int(s), msg.c_str());
    std::fflush(stdout);
  });
  weblox::WebTransport::Options opts;
  opts.connect_timeout_ms = timeout_ms;
  opts.keepalive_ms = 5'000;
  opts.idle_timeout_ms = 60'000;
  std::printf("[wt-selftest] fqdn=%s peer=%s:%u observe=%ds "
              "keepalive=5000ms idle_timeout=60000ms\n",
              fqdn.c_str(), host.c_str(), port, observe);
  std::fflush(stdout);

  if (!wt.connect(host, port, fqdn, caps, cert_hash, opts)) {
    std::printf("[summary] connect=false error=%s\n", wt.last_error().c_str());
    return 2;
  }
  auto connected_at = elapsed();
  std::printf("[observe] START t=%lldms target=%ds\n", static_cast<long long>(connected_at), observe);
  std::fflush(stdout);
  bool alive = wt.run_for(std::chrono::seconds(observe));
  double observed = (elapsed() - connected_at) / 1000.0;
  std::printf("[observe] END t=%lldms observed_s=%.2f\n",
              static_cast<long long>(elapsed()), observed);
  bool survived = alive && wt.is_open() && observed >= double(observe);
  std::printf("[summary] handshake=true h3=true connect_status=%d cert_pinned=%d "
              "survived=%d closed=%d error=%s\n",
              wt.status_code(), int(wt.cert_pin_verified()), int(survived),
              int(!wt.is_open()), wt.last_error().c_str());
  std::printf("[summary] pings=%llu packets_sent=%llu packets_recv=%llu %s\n",
              (unsigned long long)wt.keepalives_sent(),
              (unsigned long long)wt.packets_sent(),
              (unsigned long long)wt.packets_recv(),
              wt.stats_string().c_str());
  std::fflush(stdout);
  wt.close();
  return survived && wt.status_code() == 200 ? 0 : 3;
}
#endif  // WEBLOX_WT_SELFTEST
