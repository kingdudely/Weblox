// Native join-route helper. See ROUTE.md for the exact request and derivation.
#include "route.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace weblox {
namespace {

// ------------------------------------------------------------------- JSON ---
// There is no JSON library in this repository and raknet.cpp's reader lives in
// its own translation unit, so this file uses the same small strict subset:
// objects, arrays, strings (with escapes), numbers, booleans and null. Numbers
// keep their source token so ports stay exact. Bad input is rejected.

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

bool parseJson(std::string_view text, JValue& out, std::string& err) {
  JsonParser parser(text);
  if (parser.parse(out)) return true;
  err = parser.error();
  return false;
}

void appendQuoted(std::string& out, std::string_view s) {
  static const char* digits = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(digits[c >> 4]);
          out.push_back(digits[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

std::string serialize(const JValue& v) {
  std::string out;
  switch (v.type) {
    case JValue::Type::Null: out = "null"; break;
    case JValue::Type::Bool: out = v.boolean ? "true" : "false"; break;
    case JValue::Type::Number: out = v.number_text; break;
    case JValue::Type::String: appendQuoted(out, v.string); break;
    case JValue::Type::Array:
      out = "[";
      for (std::size_t i = 0; i < v.array.size(); ++i) {
        if (i) out.push_back(',');
        out += serialize(v.array[i]);
      }
      out.push_back(']');
      break;
    case JValue::Type::Object:
      out = "{";
      for (std::size_t i = 0; i < v.object.size(); ++i) {
        if (i) out.push_back(',');
        appendQuoted(out, v.object[i].first);
        out.push_back(':');
        out += serialize(v.object[i].second);
      }
      out.push_back('}');
      break;
  }
  return out;
}

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

std::optional<std::array<std::uint8_t, 16>> b64_16(const std::string& text) {
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
  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size() / 4 * 3);
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
    bytes.push_back(static_cast<std::uint8_t>(n >> 16));
    bytes.push_back(static_cast<std::uint8_t>(n >> 8));
    bytes.push_back(static_cast<std::uint8_t>(n));
  }
  if (bytes.size() < pad) return std::nullopt;
  bytes.resize(bytes.size() - pad);
  if (bytes.size() != 16) return std::nullopt;
  std::array<std::uint8_t, 16> out{};
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

std::optional<std::array<std::uint8_t, 16>> token16(const std::string& text) {
  if (text.size() == 32) {
    auto hex = hex16(text);
    if (hex) return hex;
  }
  return b64_16(text);
}

std::string toHex(const std::array<std::uint8_t, 16>& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (std::uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0xF]);
  }
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

std::string ipv4Hex(const std::array<std::uint8_t, 4>& addr) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(8);
  for (std::uint8_t b : addr) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0xF]);
  }
  return out;
}

std::string ipv4Text(const std::array<std::uint8_t, 4>& addr) {
  std::string out;
  for (int i = 0; i < 4; ++i) {
    if (i) out.push_back('.');
    out += std::to_string(static_cast<unsigned>(addr[i]));
  }
  return out;
}

// Percent-decoding only, matching Python's urllib.parse.unquote (no '+' → ' '
// translation: a raw '+' inside a base64 field must survive).
std::string urlDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%') {
      if (i + 2 >= text.size()) throw std::runtime_error("truncated percent escape in NetStackConfig");
      int hi = hexDigit(text[i + 1]);
      int lo = hexDigit(text[i + 2]);
      if (hi < 0 || lo < 0) throw std::runtime_error("bad percent escape in NetStackConfig");
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

// ------------------------------------------------------------------- HTTP ---

struct CurlInit {
  CurlInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlInit() { curl_global_cleanup(); }
};

const CurlInit& curlInit() {
  static const CurlInit init;
  return init;
}

struct Slist {
  curl_slist* head = nullptr;
  ~Slist() { curl_slist_free_all(head); }
  void add(const std::string& line) {
    head = curl_slist_append(head, line.c_str());
    if (!head) throw std::runtime_error("curl_slist_append failed");
  }
};

std::size_t appendBody(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  const std::size_t n = size * nmemb;
  static_cast<std::string*>(userdata)->append(ptr, n);
  return n;
}

bool postJoin(const FetchOptions& options, const std::string& cookie,
              const std::string& body, std::string& reply, long& http_code,
              std::string& error) {
  curlInit();
  CURL* curl = curl_easy_init();
  if (!curl) {
    error = "curl_easy_init failed";
    return false;
  }
  Slist headers;
  try {
    headers.add("Content-Type: application/json");
    headers.add("Cookie: .ROBLOSECURITY=" + cookie);
    headers.add("User-Agent: Roblox/WinInet");
    headers.add("rbx-user-agent: Roblox/Darwin");
    headers.add("Origin: https://play.roblox.com");
  } catch (const std::exception& e) {
    error = e.what();
    curl_easy_cleanup(curl);
    return false;
  }

  char errbuf[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, "https://gamejoin.roblox.com/v1/join-game");
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.head);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, options.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, options.connect_timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    error = std::string(curl_easy_strerror(rc)) +
            (errbuf[0] ? std::string(": ") + errbuf : std::string());
    curl_easy_cleanup(curl);
    return false;
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  return true;
}

std::string uuid4() {
  static std::atomic<std::uint64_t> counter{0};
  std::random_device rd;
  std::uint64_t hi = (static_cast<std::uint64_t>(rd()) << 32) | rd();
  std::uint64_t lo = (static_cast<std::uint64_t>(rd()) << 32) | rd();
  hi ^= static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  lo ^= counter.fetch_add(1, std::memory_order_relaxed) << 32;
  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL; // version 4
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL; // variant 1

  static const char* digits = "0123456789abcdef";
  std::string hex;
  hex.reserve(32);
  auto emit = [&](std::uint64_t value) {
    for (int i = 15; i >= 0; --i) hex.push_back(digits[(value >> (i * 4)) & 0xF]);
  };
  emit(hi);
  emit(lo);
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < hex.size(); ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) out.push_back('-');
    out.push_back(hex[i]);
  }
  return out;
}

std::string makeBody(std::uint64_t placeId) {
  std::string body = "{\"placeId\":" + std::to_string(placeId);
  body += ",\"isTeleport\":false";
  body += ",\"gameJoinAttemptId\":\"" + uuid4() + "\"";
  body += ",\"browserTrackerId\":0";
  body += ",\"playSessionId\":\"" + uuid4() + "\"";
  body += ",\"eventId\":\"" + uuid4() + "\"";
  body += ",\"launchData\":\"\"";
  body += ",\"joinAttemptOrigin\":\"PlayButton\"}";
  return body;
}

/// A reply that can legitimately become usable on the next POST.
struct Retryable : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::uint16_t u16From(const JValue* v, const char* name) {
  std::uint64_t n = 0;
  if (!v || !v->u64(n) || n == 0 || n > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error(std::string(name) + " missing or out of range");
  }
  return static_cast<std::uint16_t>(n);
}

JoinRoute derive(const std::string& reply, std::uint64_t placeId) {
  JValue root;
  std::string err;
  if (!parseJson(reply, root, err)) {
    throw Retryable("invalid gamejoin reply JSON: " + err);
  }

  long status = -1;
  if (const JValue* s = root.get("status")) {
    std::uint64_t v = 0;
    if (s->u64(v) && v <= static_cast<std::uint64_t>(std::numeric_limits<long>::max())) {
      status = static_cast<long>(v);
    }
  }
  if (status != 2) {
    throw Retryable("internal status " + std::to_string(status));
  }

  const JValue* js = root.get("joinScript");
  if (!js || (js->type != JValue::Type::Object && js->type != JValue::Type::String)) {
    throw Retryable("no joinScript in approved reply");
  }
  const bool was_string = js->type == JValue::Type::String;
  JValue script;
  if (was_string) {
    if (!parseJson(js->string, script, err)) {
      throw Retryable("invalid joinScript JSON: " + err);
    }
  } else {
    script = *js;
  }

  const std::string* token_text = nullptr;
  if (const JValue* t = script.get("NetStackTokenValue")) token_text = t->str();
  if (!token_text || token_text->size() < 16) {
    throw Retryable("empty NetStackTokenValue");
  }
  auto token = token16(*token_text);
  if (!token) throw Retryable("invalid NetStackTokenValue");

  const JValue* cfgv = script.get("NetStackConfig");
  JValue cfg;
  std::string cfg_text;
  if (cfgv && cfgv->type == JValue::Type::String) {
    cfg_text = urlDecode(cfgv->string); // unquote once, as in the Web Client
    if (!parseJson(cfg_text, cfg, err)) {
      throw std::runtime_error("invalid NetStackConfig JSON: " + err);
    }
  } else if (cfgv && cfgv->type == JValue::Type::Object) {
    cfg = *cfgv;
    cfg_text = serialize(*cfgv);
  } else {
    throw std::runtime_error("join script has no NetStackConfig");
  }

  const JValue* hashes = cfg.get("certHashes");
  const JValue* hash0 = hashes ? hashes->at(0) : nullptr;
  const std::string* cert = hash0 ? hash0->str() : nullptr;
  if (!cert || cert->empty()) {
    throw std::runtime_error("NetStackConfig.certHashes[0] missing");
  }
  const JValue* capsv = cfg.get("caps");
  const std::string* caps = capsv ? capsv->str() : nullptr;
  if (!caps || caps->empty()) {
    throw std::runtime_error("NetStackConfig.caps missing");
  }
  std::string pub_key;
  if (const JValue* pk = cfg.get("pubKey")) {
    if (pk->str()) pub_key = *pk->str();
  }

  const JValue* machinev = script.get("MachineAddress");
  const std::string* machine_text = machinev ? machinev->str() : nullptr;
  if (!machine_text) throw std::runtime_error("join script has no MachineAddress");
  auto machine = ipv4(*machine_text);
  if (!machine) throw std::runtime_error("bad MachineAddress: " + *machine_text);

  const std::uint16_t port = u16From(script.get("NetStackPort"), "NetStackPort");

  const JValue* endpoints = script.get("UdmuxEndpoints");
  const JValue* peer = endpoints ? endpoints->at(0) : nullptr;
  const JValue* addr = peer ? peer->get("Address") : nullptr;
  const std::string* native_text = addr ? addr->str() : nullptr;
  if (!native_text) throw std::runtime_error("join script has no UdmuxEndpoints[0].Address");
  auto native = ipv4(*native_text);
  if (!native) throw std::runtime_error("bad UdmuxEndpoints[0].Address: " + *native_text);
  if (native->back() == 255) {
    throw std::runtime_error("UdmuxEndpoints[0].Address has last octet 255");
  }
  auto browser = *native;
  browser[3] = static_cast<std::uint8_t>(browser[3] + 1);

  JoinRoute route;
  route.fqdn = toHex(*token) + "-" + ipv4Hex(*machine) + "-" +
               [&] {
                 char buf[8];
                 std::snprintf(buf, sizeof(buf), "%04x", static_cast<unsigned>(port));
                 return std::string(buf);
               }() +
               "." + ipv4Hex(browser) + ".qdmux.roblox.com";
  route.host = ipv4Text(browser);
  route.port = port;
  route.caps = *caps;
  route.cert_hash = *cert;
  route.pub_key = pub_key;
  route.join_script = was_string ? js->string : serialize(script);
  route.netstack_config = cfg_text;
  route.native_host = *native_text;
  route.native_port = u16From(peer->get("Port"), "UdmuxEndpoints[0].Port");
  route.place_id = placeId;
  if (const JValue* id = root.get("jobId")) {
    if (id->str()) route.game_id = *id->str();
  }
  route.status = status;
  return route;
}

} // namespace

JoinRoute fetch(std::uint64_t placeId, const std::string& cookie,
                const FetchOptions& options) {
  if (options.max_attempts == 0) {
    throw std::runtime_error("join fetch needs at least one attempt");
  }
  if (cookie.empty()) {
    throw std::runtime_error("empty .ROBLOSECURITY cookie");
  }
  // One attempt id for the whole retry sequence, as in the patched XHR layer.
  const std::string body = makeBody(placeId);
  std::string last = "no attempt made";

  for (unsigned attempt = 1; attempt <= options.max_attempts; ++attempt) {
    std::string reply;
    long http_code = 0;
    std::string error;
    if (!postJoin(options, cookie, body, reply, http_code, error)) {
      last = "attempt " + std::to_string(attempt) + ": " + error;
    } else if (http_code < 200 || http_code >= 300) {
      last = "attempt " + std::to_string(attempt) + ": HTTP " +
             std::to_string(http_code) + " body=" + reply.substr(0, 200);
    } else {
      try {
        return derive(reply, placeId);
      } catch (const Retryable& e) {
        last = "attempt " + std::to_string(attempt) + ": HTTP " +
               std::to_string(http_code) + ": " + e.what();
      } catch (const std::exception& e) {
        throw std::runtime_error("gamejoin reply derivation failed: " +
                                 std::string(e.what()));
      }
    }
    if (attempt < options.max_attempts && options.retry_delay_ms != 0) {
      const unsigned delay = std::min(options.retry_delay_ms * attempt, 2000u);
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
  }
  throw std::runtime_error("gamejoin route fetch failed after " +
                           std::to_string(options.max_attempts) +
                           " attempts: " + last);
}

JoinRoute fetch(std::uint64_t placeId, const std::string& cookie) {
  return fetch(placeId, cookie, FetchOptions{});
}

JoinRoute JoinRoute::fetch(std::uint64_t placeId, const std::string& cookie) {
  return weblox::fetch(placeId, cookie);
}

} // namespace weblox
