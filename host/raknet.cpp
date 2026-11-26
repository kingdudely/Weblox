// Native implementation of the RakNet datagram seam.
// Direct port of alcohol/experiment/udp-host:v2-wasmtime/src/raknet.rs with
// iwa/ring.js and iwa/rupp.js as the JS reference. See RAKNET.md.
#include "raknet.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace weblox {
namespace raknet {
namespace {

// ------------------------------------------------------------------ JSON ---
// There is no JSON library in this repository and the system nlohmann/json is
// not guaranteed on the host build, so join scripts use this small parser.
// It supports the subset JSON needs here: objects, arrays, strings (with
// escapes), numbers, booleans and null. Ports are read from the original
// number token so they stay exact; other numbers go through double. Bad input
// is rejected, never interpreted leniently.

struct JValue {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  bool integral = false;
  std::string number_text;
  std::string string;
  std::vector<JValue> array;
  std::vector<std::pair<std::string, JValue>> object;

  const JValue* get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  const JValue* at(std::size_t i) const {
    return type == Type::Array && i < array.size() ? &array[i] : nullptr;
  }
  const std::string* str() const { return type == Type::String ? &string : nullptr; }

  bool u64(std::uint64_t& out) const {
    if (type != Type::Number) return false;
    if (integral && !number_text.empty() && number_text[0] != '-') {
      errno = 0;
      char* end = nullptr;
      unsigned long long v = std::strtoull(number_text.c_str(), &end, 10);
      if (errno == 0 && end == number_text.c_str() + number_text.size()) {
        out = static_cast<std::uint64_t>(v);
        return true;
      }
    }
    if (!std::isfinite(number) || number < 1.0 ||
        number >= 18446744073709551616.0 || std::floor(number) != number) {
      return false;
    }
    out = static_cast<std::uint64_t>(number);
    return true;
  }
};

void appendUtf8(std::string& out, std::uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : s_(text) {}

  bool parse(JValue& out) {
    skipWs();
    if (!value(out)) return false;
    skipWs();
    return p_ == s_.size();
  }
  const std::string& error() const { return err_; }

 private:
  bool fail(const char* msg) {
    if (err_.empty()) err_ = msg;
    return false;
  }

  void skipWs() {
    while (p_ < s_.size() &&
           (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) {
      ++p_;
    }
  }

  bool literal(std::string_view lit, JValue& out, JValue::Type type, bool boolean) {
    if (s_.substr(p_, lit.size()) != lit) return fail("invalid literal");
    p_ += lit.size();
    out.type = type;
    out.boolean = boolean;
    return true;
  }

  static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool hex4(std::uint32_t& cp) {
    if (p_ + 4 > s_.size()) return fail("truncated \\u escape");
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      int h = hexDigit(s_[p_++]);
      if (h < 0) return fail("bad \\u escape");
      cp = (cp << 4) | static_cast<std::uint32_t>(h);
    }
    return true;
  }

  bool string(std::string& out) {
    if (p_ >= s_.size() || s_[p_] != '"') return fail("expected string");
    ++p_;
    while (p_ < s_.size()) {
      unsigned char c = static_cast<unsigned char>(s_[p_++]);
      if (c == '"') return true;
      if (c < 0x20) return fail("control character in string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (p_ >= s_.size()) return fail("truncated escape");
      char e = s_[p_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 2 <= s_.size() &&
              s_[p_] == '\\' && s_[p_ + 1] == 'u') {
            p_ += 2;
            std::uint32_t lo = 0;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          appendUtf8(out, cp);
          break;
        }
        default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }

  bool number(JValue& out) {
    const std::size_t start = p_;
    if (p_ < s_.size() && s_[p_] == '-') ++p_;
    std::size_t digits = 0;
    while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') {
      ++p_;
      ++digits;
    }
    if (digits == 0) return fail("invalid number");
    bool integral = true;
    if (p_ < s_.size() && s_[p_] == '.') {
      integral = false;
      ++p_;
      std::size_t frac = 0;
      while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') {
        ++p_;
        ++frac;
      }
      if (frac == 0) return fail("invalid fraction");
    }
    if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
      integral = false;
      ++p_;
      if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
      std::size_t exp = 0;
      while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') {
        ++p_;
        ++exp;
      }
      if (exp == 0) return fail("invalid exponent");
    }
    out.number_text.assign(s_.substr(start, p_ - start));
    out.number = std::strtod(out.number_text.c_str(), nullptr);
    out.integral = integral;
    out.type = JValue::Type::Number;
    return true;
  }

  bool array(JValue& out) {
    ++p_; // '['
    skipWs();
    if (p_ < s_.size() && s_[p_] == ']') {
      ++p_;
      out.type = JValue::Type::Array;
      return true;
    }
    for (;;) {
      JValue v;
      if (!value(v)) return false;
      out.array.push_back(std::move(v));
      skipWs();
      if (p_ >= s_.size()) return fail("unterminated array");
      if (s_[p_] == ']') {
        ++p_;
        break;
      }
      if (s_[p_] != ',') return fail("expected ',' or ']'");
      ++p_;
      skipWs();
    }
    out.type = JValue::Type::Array;
    return true;
  }

  bool object(JValue& out) {
    ++p_; // '{'
    skipWs();
    if (p_ < s_.size() && s_[p_] == '}') {
      ++p_;
      out.type = JValue::Type::Object;
      return true;
    }
    for (;;) {
      std::string key;
      if (!string(key)) return false;
      skipWs();
      if (p_ >= s_.size() || s_[p_] != ':') return fail("expected ':'");
      ++p_;
      skipWs();
      JValue v;
      if (!value(v)) return false;
      out.object.emplace_back(std::move(key), std::move(v));
      skipWs();
      if (p_ >= s_.size()) return fail("unterminated object");
      if (s_[p_] == '}') {
        ++p_;
        break;
      }
      if (s_[p_] != ',') return fail("expected ',' or '}'");
      ++p_;
      skipWs();
    }
    out.type = JValue::Type::Object;
    return true;
  }

  bool value(JValue& out) {
    if (p_ >= s_.size()) return fail("unexpected end of JSON");
    switch (s_[p_]) {
      case '{': return object(out);
      case '[': return array(out);
      case '"': {
        out.type = JValue::Type::String;
        return string(out.string);
      }
      case 't': return literal("true", out, JValue::Type::Bool, true);
      case 'f': return literal("false", out, JValue::Type::Bool, false);
      case 'n': return literal("null", out, JValue::Type::Null, false);
      default: return number(out);
    }
  }

  std::string_view s_;
  std::size_t p_ = 0;
  std::string err_;
};

// -------------------------------------------------------------- decoding ---

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::optional<std::array<std::uint8_t, 16>> hex16(const std::string& text) {
  if (text.size() != 32) return std::nullopt;
  std::array<std::uint8_t, 16> out{};
  for (std::size_t i = 0; i < 16; ++i) {
    int hi = hexDigit(text[i * 2]);
    int lo = hexDigit(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> b64Decode(const std::string& text) {
  if (text.empty() || text.size() % 4 != 0) return std::nullopt;
  auto val = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return 0;
    return -1;
  };
  std::size_t pad = 0;
  while (pad < text.size() && text[text.size() - 1 - pad] == '=') ++pad;
  if (pad > 2) return std::nullopt;
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    int a = val(static_cast<unsigned char>(text[i]));
    int b = val(static_cast<unsigned char>(text[i + 1]));
    int c = val(static_cast<unsigned char>(text[i + 2]));
    int d = val(static_cast<unsigned char>(text[i + 3]));
    if (a < 0 || b < 0 || c < 0 || d < 0) return std::nullopt;
    std::uint32_t n = (static_cast<std::uint32_t>(a) << 18) |
                      (static_cast<std::uint32_t>(b) << 12) |
                      (static_cast<std::uint32_t>(c) << 6) |
                      static_cast<std::uint32_t>(d);
    out.push_back(static_cast<std::uint8_t>(n >> 16));
    out.push_back(static_cast<std::uint8_t>(n >> 8));
    out.push_back(static_cast<std::uint8_t>(n));
  }
  if (out.size() < pad) return std::nullopt;
  out.resize(out.size() - pad);
  return out;
}

std::optional<std::array<std::uint8_t, 16>> b64_16(const std::string& text) {
  auto v = b64Decode(text);
  if (!v || v->size() != 16) return std::nullopt;
  std::array<std::uint8_t, 16> out{};
  std::copy(v->begin(), v->end(), out.begin());
  return out;
}

std::optional<std::array<std::uint8_t, 4>> ipv4(const std::string& text) {
  std::array<std::uint8_t, 4> out{};
  std::size_t n = 0;
  std::size_t start = 0;
  for (;;) {
    std::size_t dot = text.find('.', start);
    std::string part = text.substr(start, dot == std::string::npos ? std::string::npos
                                                                  : dot - start);
    if (part.empty() || part.size() > 3 || n == 4) return std::nullopt;
    unsigned value = 0;
    for (char c : part) {
      if (c < '0' || c > '9') return std::nullopt;
      value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > 255) return std::nullopt;
    out[n++] = static_cast<std::uint8_t>(value);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  if (n != 4) return std::nullopt;
  return out;
}

std::optional<std::uint16_t> port16(const JValue* v) {
  std::uint64_t n = 0;
  if (!v || !v->u64(n) || n == 0 || n > std::numeric_limits<std::uint16_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(n);
}

const JValue* firstConnection(const JValue& obj) {
  const JValue* list = obj.get("ServerConnections");
  return list ? list->at(0) : nullptr;
}

const JValue* tokenField(const JValue& obj) {
  const JValue* t = obj.get("NetStackTokenValue");
  return t ? t : obj.get("TokenValue");
}

bool decodeScript(const std::string& text, JValue& out, std::string& err) {
  JsonParser outer(text);
  JValue root;
  if (!outer.parse(root)) {
    err = outer.error();
    return false;
  }
  // joinScript is sometimes a JSON-encoded string inside the reply.
  if (root.type == JValue::Type::String) {
    JsonParser inner(root.string);
    if (!inner.parse(out)) {
      err = "joinScript: " + inner.error();
      return false;
    }
  } else {
    out = std::move(root);
  }
  return true;
}

// ------------------------------------------------------------------ RUPP ---

constexpr std::uint8_t kTlv1 = 1;
constexpr std::uint8_t kTlv2 = 2;
constexpr std::size_t kTokenTlvLen = 17; // subtype + 16-byte token
constexpr std::size_t kAddrTlvLen = 6;   // 4-byte address + 2-byte port

std::size_t tlvEnd(const std::uint8_t* d, std::size_t len) {
  if (!d || len < 8 || d[0] != 0x01 || d[1] != 0x00) return 0;
  std::size_t total = (static_cast<std::size_t>(d[2]) << 8) | d[3];
  if (total < 4 || total > len) return 0;
  return total;
}

} // namespace

// ------------------------------------------------------------ join script ---

JoinTarget parseJoin(const std::string& script) {
  JValue obj;
  std::string err;
  if (!decodeScript(script, obj, err)) throw std::runtime_error(err);

  const JValue* endpoints = obj.get("UdmuxEndpoints");
  const JValue* peer = endpoints ? endpoints->at(0) : nullptr;
  if (!peer) peer = firstConnection(obj);
  if (!peer) throw std::runtime_error("no peer endpoint");

  const std::string* address = nullptr;
  if (const JValue* a = peer->get("Address")) address = a->str();
  if (!address) throw std::runtime_error("peer has no Address");

  // Accept IPv4 and IPv6 literals, like Rust's IpAddr::from_str.
  bool valid_ip = false;
  struct in_addr a4 {};
  struct in6_addr a6 {};
  if (::inet_pton(AF_INET, address->c_str(), &a4) == 1) {
    valid_ip = true;
  } else if (::inet_pton(AF_INET6, address->c_str(), &a6) == 1) {
    valid_ip = true;
  }
  if (!valid_ip) {
    throw std::runtime_error("bad peer Address '" + *address +
                             "': expected IPv4 or IPv6 address");
  }
  auto port = port16(peer->get("Port"));
  if (!port) throw std::runtime_error("peer has no Port");

  JoinTarget out;
  out.host = *address;
  out.port = *port;
  const JValue* machine = obj.get("MachineAddress");
  if (machine && machine->str()) out.machine = ipv4(*machine->str());
  out.netstack_port = port16(obj.get("NetStackPort"));
  const JValue* tok = tokenField(obj);
  if (tok && tok->str()) {
    out.token = tok->str()->size() == 32 ? hex16(*tok->str()) : b64_16(*tok->str());
  }
  const JValue* conn = firstConnection(obj);
  out.data_port = port16(conn ? conn->get("Port") : nullptr);
  if (!out.data_port) out.data_port = port16(obj.get("ServerPort"));
  return out;
}

Rupp& Rupp::setJoinScript(const std::string& script) {
  JValue obj;
  std::string err;
  if (!decodeScript(script, obj, err)) return *this;

  if (!join_) {
    const JValue* tok = tokenField(obj);
    if (tok && tok->str()) {
      join_ = tok->str()->size() == 32 ? hex16(*tok->str()) : b64_16(*tok->str());
    }
  }
  if (!rcc_) {
    const JValue* conn = firstConnection(obj);
    const std::string* addr_text = nullptr;
    if (const JValue* a = obj.get("MachineAddress")) addr_text = a->str();
    if (!addr_text && conn) {
      if (const JValue* a = conn->get("Address")) addr_text = a->str();
    }
    auto netstack = port16(obj.get("NetStackPort"));
    if (addr_text && netstack) {
      auto addr = ipv4(*addr_text);
      if (addr) rcc_ = std::make_pair(*addr, *netstack);
    }
  }
  if (data_port_ == 0) {
    const JValue* conn = firstConnection(obj);
    auto port = port16(conn ? conn->get("Port") : nullptr);
    if (!port) port = port16(obj.get("ServerPort"));
    if (port) data_port_ = *port;
  }
  return *this;
}

void Rupp::stamp(std::uint8_t* d, std::size_t len) {
  if (!d || (!join_ && !rcc_)) return;
  const std::size_t end = tlvEnd(d, len);
  if (end == 0) return;
  std::size_t p = 4;
  std::uint8_t sub = 0;
  while (p + 2 <= end) {
    std::uint8_t kind = d[p];
    std::size_t tlen = d[p + 1];
    if (p + 2 + tlen > end) break;
    if (kind == kTlv1 && tlen == kTokenTlvLen) {
      sub = d[p + 2];
      if (sub == 1 && join_) {
        std::copy(join_->begin(), join_->end(), d + p + 3);
      } else if (sub == 2 && session_) {
        std::copy(session_->begin(), session_->end(), d + p + 3);
      }
    } else if (kind == kTlv2 && tlen == kAddrTlvLen && rcc_) {
      // NetStackPort is only right while handshaking; the established session
      // is routed by ServerConnections[0].Port.
      std::uint16_t port = (sub == 2 && data_port_ != 0) ? data_port_ : rcc_->second;
      std::copy(rcc_->first.begin(), rcc_->first.end(), d + p + 2);
      d[p + 6] = static_cast<std::uint8_t>(port >> 8);
      d[p + 7] = static_cast<std::uint8_t>(port & 0xFF);
    }
    p += 2 + tlen;
  }
}

void Rupp::learn(const std::uint8_t* d, std::size_t len) {
  const std::size_t end = tlvEnd(d, len);
  if (end == 0) return;
  std::size_t p = 4;
  while (p + 2 <= end) {
    std::uint8_t kind = d[p];
    std::size_t tlen = d[p + 1];
    if (p + 2 + tlen > end) return;
    if (kind == kTlv1 && tlen == kTokenTlvLen && d[p + 2] == 2) {
      std::array<std::uint8_t, 16> token{};
      std::copy(d + p + 3, d + p + 19, token.begin());
      session_ = token;
      return;
    }
    p += 2 + tlen;
  }
}

// ------------------------------------------------------------------- ring ---

bool debug() {
  static const bool enabled = ::getenv("WC_RAKNET_DBG") != nullptr;
  return enabled;
}

class Ring {
 public:
  Ring(std::uint8_t* base, std::size_t size, std::uint32_t slot)
      : base_(base), size_(size), slot_(slot), rx0_(kTx0 + kNSlot * slot) {}

  std::uint32_t slot() const { return slot_; }
  std::uint32_t rx0() const { return rx0_; }

  std::uint32_t loadAt(std::uint32_t addr) const {
    auto* w = word(addr);
    return w ? w->load(std::memory_order_acquire) : 0;
  }
  std::uint32_t load(std::uint32_t off) const { return loadAt(kRingBase + off); }

  void storeAt(std::uint32_t addr, std::uint32_t v) const {
    if (auto* w = word(addr)) {
      w->store(v, std::memory_order_release);
    } else if (debug()) {
      std::fprintf(stderr, "[rk ] write out of bounds at %#x\n", addr);
    }
  }
  void store(std::uint32_t off, std::uint32_t v) const { storeAt(kRingBase + off, v); }

  std::size_t readAt(std::uint32_t addr, std::uint8_t* dst, std::size_t len) const {
    if (!dst || addr >= size_) return 0;
    std::size_t n = std::min({len, kMaxDatagram, size_ - static_cast<std::size_t>(addr)});
    if (n != 0) std::memcpy(dst, base_ + addr, n);
    return n;
  }

  std::size_t writeAt(std::uint32_t addr, const std::uint8_t* src, std::size_t len) const {
    if (!src || addr >= size_) return 0;
    std::size_t n = std::min(len, size_ - static_cast<std::size_t>(addr));
    if (n != 0) std::memcpy(base_ + addr, src, n);
    return n;
  }

  void install(const std::string& host, std::uint16_t port) const {
    std::array<std::uint8_t, kHostLen> buf{};
    std::size_t n = std::min(host.size(), kHostLen - 1);
    std::memcpy(buf.data(), host.data(), n);
    writeAt(kRingBase + kHost, buf.data(), buf.size());
    store(kPort, port);
    store(kCreq, 0);
    store(kCres, static_cast<std::uint32_t>(-12345));
    // MAGIC is written last: it is our "pump is live" flag.
    store(kMagic, kMagicValue);
    if (debug()) {
      std::fprintf(stderr, "[rk ] route %s:%u slot=%u rx0=%#x\n", host.c_str(), port,
                   slot_, rx0_);
    }
  }

  void armConnect() const { store(kCreq, 1); }

 private:
  // The guest performs wasm memory.atomic.* on these aligned 32-bit words.
  // Guest memory is page-aligned in practice; check alignment before treating
  // a word as std::atomic<uint32_t> (same approach as the Rust port).
  std::atomic<std::uint32_t>* word(std::uint32_t addr) const {
    if ((addr & 3u) != 0 || static_cast<std::size_t>(addr) + 4 > size_) return nullptr;
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(base_) + addr;
    if ((a & 3u) != 0) return nullptr;
    return reinterpret_cast<std::atomic<std::uint32_t>*>(base_ + addr);
  }

  std::uint8_t* base_;
  std::size_t size_;
  std::uint32_t slot_;
  std::uint32_t rx0_;
};

// ------------------------------------------------------------------- pump ---

struct SocketFd {
  int fd = -1;
  ~SocketFd() {
    if (fd >= 0) ::close(fd);
  }
  SocketFd() = default;
  SocketFd(const SocketFd&) = delete;
  SocketFd& operator=(const SocketFd&) = delete;
};

struct Session {
  SocketFd sock;
  sockaddr_storage peer{};
  socklen_t peer_len = 0;
  std::unique_ptr<Ring> ring;
  std::shared_ptr<Rupp> rupp;
  std::mutex rupp_mu;
  std::function<void()> wake;
  std::thread tx;
  std::thread rx;
  std::atomic<std::uint64_t> tx_count{0};
  std::atomic<std::uint64_t> rx_count{0};
  std::atomic<std::uint64_t> drop_count{0};
  std::atomic<std::uint64_t> error_count{0};
};

std::mutex g_session_mu;
std::unique_ptr<Session> g_session;
Stats g_last_stats{}; // keep counters readable after shutdown, like the Rust globals
std::atomic<bool> g_started{false};
std::atomic<bool> g_stop{false};

std::uint32_t slotBase(const Ring& ring, bool tx, std::uint32_t seq) {
  std::uint32_t start = tx ? kTx0 : ring.rx0();
  return kRingBase + start + (seq & (kNSlot - 1)) * ring.slot();
}

void txLoop(Session& s) {
  std::uint32_t last_cres = s.ring->load(kCres);
  std::uint32_t idle_ms = 2;
  while (!g_stop.load(std::memory_order_acquire)) {
    std::uint32_t head = s.ring->load(kTxHead);
    std::uint32_t tail = s.ring->load(kTxTail);
    std::uint32_t sent = 0;
    while (tail != head && !g_stop.load(std::memory_order_acquire)) {
      std::uint32_t base = slotBase(*s.ring, true, tail);
      std::uint32_t len = s.ring->loadAt(base);
      // Zero means the guest has not finished publishing this slot.
      if (len == 0 || len > kMaxDatagram) break;
      std::vector<std::uint8_t> dgram(len);
      if (s.ring->readAt(base + 4, dgram.data(), len) != len) break;
      {
        std::lock_guard<std::mutex> lock(s.rupp_mu);
        s.rupp->stamp(dgram);
      }
      if (::sendto(s.sock.fd, dgram.data(), dgram.size(), MSG_NOSIGNAL,
                   reinterpret_cast<const sockaddr*>(&s.peer), s.peer_len) < 0) {
        s.error_count.fetch_add(1, std::memory_order_relaxed);
        if (debug()) std::fprintf(stderr, "[rk ] send failed: %s\n", std::strerror(errno));
        break;
      }
      tail = tail + 1;
      s.ring->store(kTxTail, tail); // publish consumption last
      s.tx_count.fetch_add(1, std::memory_order_relaxed);
      ++sent;
    }
    std::uint32_t cres = s.ring->load(kCres);
    if (cres != last_cres && cres != static_cast<std::uint32_t>(-12345)) {
      std::fprintf(stderr, "[rk ] Connect result %d\n", static_cast<std::int32_t>(cres));
      last_cres = cres;
    }
    if (sent == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
      idle_ms = std::min(idle_ms + 1, 10u);
    } else {
      idle_ms = 2;
    }
  }
}

void rxLoop(Session& s) {
  std::vector<std::uint8_t> buf(kMaxDatagram);
  while (!g_stop.load(std::memory_order_acquire)) {
    sockaddr_storage from{};
    socklen_t from_len = sizeof(from);
    ssize_t n = ::recvfrom(s.sock.fd, buf.data(), buf.size(), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      s.error_count.fetch_add(1, std::memory_order_relaxed);
      if (debug()) std::fprintf(stderr, "[rk ] recv failed: %s\n", std::strerror(errno));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (n == 0) continue;
    {
      std::lock_guard<std::mutex> lock(s.rupp_mu);
      s.rupp->learn(buf.data(), static_cast<std::size_t>(n));
    }
    std::uint32_t head = s.ring->load(kRxHead);
    std::uint32_t tail = s.ring->load(kRxTail);
    if (head - tail >= kNSlot) {
      s.drop_count.fetch_add(1, std::memory_order_relaxed);
      if (debug()) std::fprintf(stderr, "[rk ] RX ring full\n");
      continue;
    }
    std::uint32_t base = slotBase(*s.ring, false, head);
    std::size_t len = std::min(static_cast<std::size_t>(n), kMaxDatagram);
    // Publish length last: the guest's recv stub treats zero as empty.
    s.ring->storeAt(base, 0);
    s.ring->writeAt(base + 4, buf.data(), len);
    s.ring->storeAt(base, static_cast<std::uint32_t>(len));
    s.ring->store(kRxHead, head + 1);
    if (s.wake) s.wake(); // wake the guest's select()/recv thread
    s.rx_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void bindSocket(Session& s, const std::string& host, std::uint16_t port, int family) {
  if (family == AF_INET) {
    struct in_addr addr {};
    if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) {
      throw std::runtime_error("bad IPv4 peer address: " + host);
    }
    auto* peer = reinterpret_cast<sockaddr_in*>(&s.peer);
    std::memset(peer, 0, sizeof(*peer));
    peer->sin_family = AF_INET;
    peer->sin_port = htons(port);
    peer->sin_addr = addr;
    s.peer_len = sizeof(*peer);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0; // ephemeral, kept for the whole session
    if (::bind(s.sock.fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
      throw std::system_error(errno, std::generic_category(), "bind UDP socket");
    }
  } else {
    struct in6_addr addr {};
    if (::inet_pton(AF_INET6, host.c_str(), &addr) != 1) {
      throw std::runtime_error("bad IPv6 peer address: " + host);
    }
    auto* peer = reinterpret_cast<sockaddr_in6*>(&s.peer);
    std::memset(peer, 0, sizeof(*peer));
    peer->sin6_family = AF_INET6;
    peer->sin6_port = htons(port);
    peer->sin6_addr = addr;
    s.peer_len = sizeof(*peer);

    sockaddr_in6 local{};
    local.sin6_family = AF_INET6;
    local.sin6_addr = in6addr_any;
    local.sin6_port = 0;
    if (::bind(s.sock.fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
      throw std::system_error(errno, std::generic_category(), "bind UDP6 socket");
    }
  }
  timeval tv{0, 250000}; // 250 ms, as in the Rust port
  if (::setsockopt(s.sock.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
      ::setsockopt(s.sock.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
    throw std::system_error(errno, std::generic_category(), "set UDP timeout");
  }
}

void startImpl(const std::string& host_in, std::uint16_t port_in,
               const std::string& script, std::uint8_t* base,
               std::function<void()> wake, const StartOptions& options) {
  if (!base) throw std::runtime_error("RakNet start needs guest linear memory");

  std::string host = host_in;
  std::uint16_t port = port_in;
  JoinTarget target;
  bool have_target = false;
  if (!script.empty()) {
    try {
      target = parseJoin(script);
      have_target = true;
    } catch (const std::exception& e) {
      // start() can be called with an explicit route and no usable script;
      // in that case the RUPP just stays unstamped.
      if (host.empty() || port == 0) throw;
      if (debug()) std::fprintf(stderr, "[rk ] join script ignored: %s\n", e.what());
    }
  }
  if (host.empty()) {
    if (!have_target) throw std::runtime_error("RakNet start needs a host or join script");
    host = target.host;
  }
  if (port == 0) {
    if (!have_target) throw std::runtime_error("RakNet start needs a port or join script");
    port = target.port;
  }

  int family = AF_INET;
  struct in_addr a4 {};
  struct in6_addr a6 {};
  if (::inet_pton(AF_INET, host.c_str(), &a4) == 1) {
    family = AF_INET;
  } else if (::inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
    family = AF_INET6;
  } else {
    throw std::runtime_error("bad peer Address '" + host +
                             "': expected IPv4 or IPv6 address");
  }

  std::uint32_t slot = options.slot;
  if (slot == 0) {
    if (const char* env = ::getenv("WC_RK_SLOT")) {
      char* end = nullptr;
      unsigned long v = std::strtoul(env, &end, 10);
      if (end != env && *end == '\0') slot = static_cast<std::uint32_t>(
          std::min<unsigned long>(std::max<unsigned long>(v, 8), 64 * 1024));
    }
    if (slot == 0) slot = kSlot;
  }
  // The Rust port clamps to 8..64 KiB; also keep a full datagram inside one
  // slot so a larger MAX_DATAGRAM can never spill into the next slot.
  slot = std::max(slot, static_cast<std::uint32_t>(kMaxDatagram + 4));
  std::uint64_t end = static_cast<std::uint64_t>(kRingBase) + kTx0 +
                      static_cast<std::uint64_t>(kNSlot) * slot * 2;
  if (end > options.memory_size) {
    throw std::runtime_error("ring does not fit in guest memory");
  }

  auto s = std::make_unique<Session>();
  s->sock.fd = ::socket(family, SOCK_DGRAM, 0);
  if (s->sock.fd < 0) {
    throw std::system_error(errno, std::generic_category(), "create UDP socket");
  }
  bindSocket(*s, host, port, family);
  s->ring = std::make_unique<Ring>(base, options.memory_size, slot);
  s->ring->install(host, port);
  s->wake = std::move(wake);
  s->rupp = std::make_shared<Rupp>();
  s->rupp->setJoinScript(script);
  if (debug()) {
    std::string token;
    if (s->rupp->join()) {
      for (std::uint8_t b : *s->rupp->join()) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        token += hex;
      }
    }
    std::fprintf(stderr, "[rk ] join token=%s data=%u\n", token.c_str(),
                 s->rupp->data_port());
  }

  {
    std::lock_guard<std::mutex> lock(g_session_mu);
    g_session = std::move(s);
  }
  Session& session = *g_session;
  try {
    session.tx = std::thread([&session] { txLoop(session); });
    session.rx = std::thread([&session] { rxLoop(session); });
    ::pthread_setname_np(session.tx.native_handle(), "rk-tx");
    ::pthread_setname_np(session.rx.native_handle(), "rk-rx");
  } catch (...) {
    stop();
    shutdown();
    throw;
  }
  if (options.arm_connect) session.ring->armConnect();

  sockaddr_in local{};
  socklen_t local_len = sizeof(local);
  unsigned local_port = 0;
  if (::getsockname(session.sock.fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
    local_port = ntohs(local.sin_port);
  }
  std::fprintf(stderr, "[rk ] udp %s:%u local %u (%u slots x %u B)\n", host.c_str(),
               port, local_port, kNSlot, session.ring->slot());
}

Stats stats() {
  std::lock_guard<std::mutex> lock(g_session_mu);
  if (!g_session) return g_last_stats;
  g_last_stats.tx = g_session->tx_count.load(std::memory_order_relaxed);
  g_last_stats.rx = g_session->rx_count.load(std::memory_order_relaxed);
  g_last_stats.dropped = g_session->drop_count.load(std::memory_order_relaxed);
  g_last_stats.errors = g_session->error_count.load(std::memory_order_relaxed);
  return g_last_stats;
}

void stop() { g_stop.store(true, std::memory_order_release); }

void shutdown() {
  stop();
  std::unique_ptr<Session> s;
  {
    std::lock_guard<std::mutex> lock(g_session_mu);
    s = std::move(g_session);
  }
  if (!s) {
    g_started.store(false, std::memory_order_release);
    return;
  }
  std::thread::id self = std::this_thread::get_id();
  if (s->tx.joinable() && s->tx.get_id() != self) s->tx.join();
  if (s->rx.joinable() && s->rx.get_id() != self) s->rx.join();
  g_last_stats.tx = s->tx_count.load(std::memory_order_relaxed);
  g_last_stats.rx = s->rx_count.load(std::memory_order_relaxed);
  g_last_stats.dropped = s->drop_count.load(std::memory_order_relaxed);
  g_last_stats.errors = s->error_count.load(std::memory_order_relaxed);
  s.reset(); // closes the UDP socket
  g_started.store(false, std::memory_order_release);
}

void start(const std::string& host, std::uint16_t port, const std::string& script,
           std::uint8_t* guest_memory_base, std::function<void()> wake,
           const StartOptions& options) {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    throw std::runtime_error("RakNet ring already started");
  }
  g_stop.store(false, std::memory_order_release);
  try {
    startImpl(host, port, script, guest_memory_base, std::move(wake), options);
  } catch (...) {
    shutdown();
    throw;
  }
}

} // namespace raknet
} // namespace weblox
