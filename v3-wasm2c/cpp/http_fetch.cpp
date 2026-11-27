// Emscripten_fetch imports and the join POST (ported from v3-wasm2c/http.c).
//
// Browser glue builds an `emscripten_fetch_t` in guest memory and asks its
// fetchXHR shim to fill it in.  We do the same synchronously with libcurl, then
// dispatch the guest callbacks exactly as fetchXHR's completion handler does.
//
#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// host.cpp owns the instantiated module; imports receive only `struct w2c_env`
// (the import table), so the WebClient instance comes from this global.
extern "C" {
extern w2c_WebClient* g_wc_instance;
}

namespace weblox::deferred {
void detail_push(u32 fn, u32 arg);
}

// ---------------------------------------------------------------------------
// GUEST STRUCT LAYOUT (single source of truth for every offset below)
// ---------------------------------------------------------------------------
// These are the offsets of the glue's `emscripten_fetch_t` / its attribute
// block, as documented at the top of v3-wasm2c/http.c (the struct is generated
// by WebClient.js's fetchXHR, not by WC.h).  They are kept in one block so an
// engine update changes one place instead of scattered magic numbers.
//   fetch object: url ptr +8; data +12; numBytes +16 (u64); dataOffset +24
//                 (u64); total +32 (u64); readyState +40 (u16);
//                 status +42 (u16); statusText +44 (char[64]).
//   attribute block at fetch +108: method[32] +0; onsuccess +36; onerror +40;
//                 onprogress +44; onreadystatechange +48; attributes +52;
//                 timeout +56; withCredentials +60; userName +68;
//                 password +72; headers +76 (pairs of u32, null terminated);
//                 mime +80; body ptr +84; body size +88 (u32).
namespace {

constexpr u32 kFetchUrlPtr        = 8;
constexpr u32 kFetchDataPtr       = 12;
constexpr u32 kFetchNumBytes      = 16;
constexpr u32 kFetchDataOffset    = 24;
constexpr u32 kFetchTotalBytes    = 32;
constexpr u32 kFetchReadyState    = 40;
constexpr u32 kFetchStatus        = 42;
constexpr u32 kFetchStatusText    = 44;
constexpr u32 kFetchStatusTextCap = 64;
constexpr u32 kFetchAttrs         = 108;

constexpr u32 kAttrMethod         = 0;   // char[32]
constexpr u32 kAttrOnSuccess      = 36;
constexpr u32 kAttrOnError        = 40;
[[maybe_unused]] constexpr u32 kAttrOnProgress = 44;
constexpr u32 kAttrReadyChange    = 48;
[[maybe_unused]] constexpr u32 kAttrAttributes = 52;
[[maybe_unused]] constexpr u32 kAttrTimeout = 56;
[[maybe_unused]] constexpr u32 kAttrCredentials = 60;
[[maybe_unused]] constexpr u32 kAttrUserName = 68;
[[maybe_unused]] constexpr u32 kAttrPassword = 72;
constexpr u32 kAttrHeaders        = 76;
[[maybe_unused]] constexpr u32 kAttrMime = 80;
constexpr u32 kAttrBodyPtr        = 84;
constexpr u32 kAttrBodyLen        = 88;

constexpr std::size_t kMaxBody = 256u << 20;
constexpr std::uint32_t kTimeoutMs = 120000;  // curl CLI used --max-time 120

}  // namespace

namespace weblox::http {

struct Response {
  long status = 0;       // 0 means transport failure (XHR's NetworkError)
  std::string body;
  std::string headers;   // raw header block, as XHR's getAllResponseHeaders
};

namespace {

std::once_flag g_curl_once;
void ensure_curl() {
  std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct BodySink {
  std::string data;
  std::size_t max = kMaxBody;
};

std::size_t append_body(char* data, std::size_t size, std::size_t count,
                        void* user) {
  auto* sink = static_cast<BodySink*>(user);
  const std::size_t n = size * count;
  if (n > sink->max - std::min(sink->data.size(), sink->max)) return 0;
  sink->data.append(data, n);
  return n;
}

std::size_t append_header(char* data, std::size_t size, std::size_t count,
                          void* user) {
  auto* text = static_cast<std::string*>(user);
  text->append(data, size * count);
  return size * count;
}

std::string trim_line_end(std::string value) {
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  return value;
}

std::string cookie_value() {
  static const std::string value = [] {
    const char* env = std::getenv("WC_COOKIE");
    if (env && *env) return trim_line_end(env);
    const char* file = std::getenv("WC_COOKIE_FILE");
    std::ifstream in((file && *file) ? file : "king.txt", std::ios::binary);
    if (!in) return std::string();
    std::string text;
    text.resize(4096);
    in.read(&text[0], 4095);
    text.resize(static_cast<std::size_t>(in.gcount()));
    return trim_line_end(text);
  }();
  return value;
}

bool iequals(const std::string& a, const char* b) {
  std::size_t i = 0;
  for (; i < a.size() && b[i]; ++i) {
    const int x = std::tolower(static_cast<unsigned char>(a[i]));
    const int y = std::tolower(static_cast<unsigned char>(b[i]));
    if (x != y) return false;
  }
  return i == a.size() && !b[i];
}

std::string authority(const std::string& url) {
  std::size_t at = url.find("://");
  at = (at == std::string::npos) ? 0 : at + 3;
  const std::size_t end = url.find_first_of("/:?#", at);
  return url.substr(at, end == std::string::npos ? end : end - at);
}

}  // namespace

// One synchronous request.  Redirects are followed and the status is the final
// HTTP code; a transport error leaves status at 0, which maps to XHR's error
// callback rather than to a 4xx response.
Response request(const std::string& method, const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body, std::size_t max_bytes,
                 std::uint32_t timeout_ms) {
  ensure_curl();
  Response out;
  CURL* curl = curl_easy_init();
  if (!curl) return out;

  curl_slist* list = nullptr;
  // curl otherwise adds `Expect: 100-continue` to large POSTs; the browser
  // fetch path never sends it and some join endpoints reject it.
  list = curl_slist_append(list, "Expect:");
  for (const auto& header : headers)
    list = curl_slist_append(list, (header.first + ": " + header.second).c_str());

  BodySink sink;
  sink.max = max_bytes;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, append_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.headers);
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
  }

  if (curl_easy_perform(curl) == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
  out.body = std::move(sink.data);

  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return out;
}

}  // namespace weblox::http

namespace {

// The URL/header helpers live next to the curl request below; bring them into
// scope for the import entry points in this translation unit.
using ::weblox::http::authority;
using ::weblox::http::cookie_value;
using ::weblox::http::iequals;

// ------------------------------------------------------------- response data

std::mutex g_headers_mutex;
std::unordered_map<u32, std::string> g_headers;

void set_headers(u32 fetch, std::string text) {
  std::lock_guard<std::mutex> lock(g_headers_mutex);
  if (text.empty())
    g_headers.erase(fetch);
  else
    g_headers[fetch] = std::move(text);
}

std::string get_headers(u32 fetch) {
  std::lock_guard<std::mutex> lock(g_headers_mutex);
  const auto it = g_headers.find(fetch);
  return it == g_headers.end() ? std::string() : it->second;
}

u32 read_u32(u32 address) {
  u32 value = 0;
  if (const auto* p = ::weblox::guest::ptr(address, sizeof value))
    std::memcpy(&value, p, sizeof value);
  return value;
}

void write_u16(u32 address, u16 value) {
  if (auto* p = ::weblox::guest::ptr(address, sizeof value))
    std::memcpy(p, &value, sizeof value);
}

void write_text(u32 address, std::size_t capacity, const std::string& text) {
  auto* p = ::weblox::guest::ptr(address, capacity);
  if (!p) return;
  const std::size_t n = std::min(text.size(), capacity - 1);
  std::memcpy(p, text.data(), n);
  p[n] = 0;
}

// XHR/fetch completion handlers run as browser tasks, not inside the import
// that started the request.  Queueing them avoids re-entering an RBX_on*
// callback that is still on the wasm stack.
void call_callback(u32 fetch, u32 index) {
  if (!index || !g_wc_instance) return;
  auto& table = g_wc_instance->w2c_0x5F_indirect_function_table;
  if (index >= table.size || !table.data[index].func) return;
  ::weblox::deferred::detail_push(index, fetch);
}

// ------------------------------------------------------------------- join ---

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool json_value(const std::string& json, const char* key, std::size_t& pos) {
  const std::string pattern = std::string("\"") + key + "\":";
  const std::size_t at = json.find(pattern);
  if (at == std::string::npos) return false;
  pos = at + pattern.size();
  while (pos < json.size() && json[pos] == ' ') ++pos;
  return true;
}

// The join token is a base64 blob in the reply but a 32-char lowercase hex
// string in the FQDN.  Accept either (the extension's transform).
bool token_hex(const std::string& raw, char out[33]) {
  std::size_t begin = 0;
  std::size_t end = raw.size();
  if (!raw.empty() && raw[0] == '"') {
    begin = 1;
    const std::size_t quote = raw.find('"', begin);
    if (quote != std::string::npos) end = quote;
  }
  while (begin < end && raw[begin] == ' ') ++begin;
  const std::string value = raw.substr(begin, end - begin);

  if (value.size() == 32) {
    bool all_hex = true;
    for (char c : value) all_hex = all_hex && hex_value(c) >= 0;
    if (all_hex) {
      for (std::size_t i = 0; i < 32; ++i)
        out[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(value[i])));
      out[32] = 0;
      return true;
    }
  }

  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned char bytes[16];
  int count = 0, bits = 0, acc = 0;
  for (char c : value) {
    if (c == '=') break;
    const char* digit = std::strchr(alphabet, c);
    if (!digit) return false;
    acc = (acc << 6) | static_cast<int>(digit - alphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (count < 16) bytes[count++] = static_cast<unsigned char>((acc >> bits) & 0xff);
    }
  }
  if (count != 16) return false;
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) {
    out[i * 2] = hex[bytes[i] >> 4];
    out[i * 2 + 1] = hex[bytes[i] & 15];
  }
  out[32] = 0;
  return true;
}

bool ipv4_hex(const std::string& raw, char out[9], int& last_octet) {
  int octet[4] = {0, 0, 0, 0};
  if (std::sscanf(raw.c_str(), "%d.%d.%d.%d", &octet[0], &octet[1],
                  &octet[2], &octet[3]) != 4)
    return false;
  for (int value : octet)
    if (value < 0 || value > 255) return false;
  std::snprintf(out, 9, "%02x%02x%02x%02x", octet[0], octet[1], octet[2],
                octet[3]);
  last_octet = octet[3];
  return true;
}

// The browser endpoint lives at the odd/even neighbour of the native UDMUX
// address; only odd, non-.255 sources have one.  Layout from http.c:
//   token-hex - rcc-ip-hex - netstack-port-hex . (udmux last octet + 1) . qdmux
bool derive_fqdn(const std::string& json, std::string& fqdn) {
  std::size_t token_at = 0, address_at = 0, port_at = 0, endpoints_at = 0;
  if (!json_value(json, "NetStackTokenValue", token_at) &&
      !json_value(json, "TokenValue", token_at))
    return false;
  if (!json_value(json, "MachineAddress", address_at) ||
      !json_value(json, "NetStackPort", port_at))
    return false;
  endpoints_at = json.find("\"UdmuxEndpoints\"");
  if (endpoints_at == std::string::npos) return false;
  endpoints_at = json.find("\"Address\":", endpoints_at);
  if (endpoints_at == std::string::npos) return false;
  endpoints_at += std::strlen("\"Address\":");
  while (endpoints_at < json.size() && json[endpoints_at] == ' ')
    ++endpoints_at;
  if (endpoints_at >= json.size() || json[endpoints_at] != '"') return false;
  const std::size_t quote = json.find('"', endpoints_at + 1);
  if (quote == std::string::npos) return false;

  char token[33], rcc[9], udmux[9];
  int rcc_last = 0, udmux_last = 0;
  if (!token_hex(json.substr(token_at), token)) return false;
  // json_value leaves us on the opening quote; skip it before looking for the
  // closing one (otherwise the address would parse as an empty string).
  const std::size_t address_start =
      (address_at < json.size() && json[address_at] == '"') ? address_at + 1
                                                            : address_at;
  const std::size_t address_end = json.find('"', address_start);
  if (address_end == std::string::npos) return false;
  if (!ipv4_hex(json.substr(address_start, address_end - address_start), rcc,
                rcc_last))
    return false;
  if (!ipv4_hex(json.substr(endpoints_at + 1, quote - endpoints_at - 1),
                udmux, udmux_last))
    return false;
  if ((udmux_last & 1) == 0 || udmux_last == 255) return false;

  const long port = std::strtol(json.substr(port_at).c_str(), nullptr, 10);
  if (port < 1 || port > 65535) return false;
  udmux[6] = "0123456789abcdef"[(udmux_last + 1) >> 4];
  udmux[7] = "0123456789abcdef"[(udmux_last + 1) & 15];

  char buffer[160];
  std::snprintf(buffer, sizeof buffer, "%s-%s-%04lx.%s.qdmux.roblox.com",
                token, rcc, port, udmux);
  fqdn = buffer;
  return true;
}

std::string patch_join(const std::string& body, std::string& fqdn) {
  const std::size_t script = body.find("\"joinScript\"");
  if (script == std::string::npos) return body;
  const std::size_t open = body.find('{', script);
  if (open == std::string::npos) return body;
  // Only an approved reply has the network fields we derive from.
  if (!derive_fqdn(body.substr(open), fqdn)) return body;

  const std::size_t field = body.find("\"gameFqdn\"", open);
  if (field != std::string::npos) {
    const std::size_t colon = body.find(':', field + 10);
    if (colon != std::string::npos) {
      std::size_t value = colon + 1;
      while (value < body.size() && body[value] == ' ') ++value;
      if (body.compare(value, 4, "null") == 0) {
        std::string out = body;
        out.replace(value, 4, "\"" + fqdn + "\"");
        return out;
      }
      if (value < body.size() && body[value] == '"') {
        const std::size_t end = body.find('"', value + 1);
        if (end != std::string::npos && end == value + 1) {
          std::string out = body;
          out.replace(value, 2, "\"" + fqdn + "\"");
          return out;
        }
        return body;  // the server already supplied a host
      }
    }
  }

  std::string out = body;
  out.insert(open + 1, "\"gameFqdn\":\"" + fqdn + "\",");
  return out;
}

bool is_host(const std::string& url, const char* want) {
  return iequals(authority(url), want);
}

}  // namespace

extern "C" {

void w2c_env_emscripten_start_fetch(struct w2c_env*, u32 fetch) {
  const std::string url = ::weblox::guest::string(read_u32(fetch + kFetchUrlPtr));
  const u32 attrs = fetch + kFetchAttrs;
  std::string method = ::weblox::guest::string(attrs + kAttrMethod);
  const u32 on_ready = read_u32(attrs + kAttrReadyChange);
  const u32 on_success = read_u32(attrs + kAttrOnSuccess);
  const u32 on_error = read_u32(attrs + kAttrOnError);

  if (url.empty()) {
    set_headers(fetch, {});
    call_callback(fetch, on_error);
    return;
  }
  if (method.empty()) method = "GET";
  std::fprintf(stderr, "[http] %s %s\n", method.c_str(), url.c_str());

  const bool join = is_host(url, "gamejoin.roblox.com") &&
                    url.find("/join") != std::string::npos;
  const bool roblox =
      is_host(url, "roblox.com") || url.find(".roblox.com") != std::string::npos;

  std::vector<std::pair<std::string, std::string>> headers;
  if (roblox) {
    const std::string cookie = cookie_value();
    if (!cookie.empty()) headers.emplace_back("Cookie", ".ROBLOSECURITY=" + cookie);
  }
  if (join) {
    // Native identity, otherwise gamejoin answers status 12 "Playtesters Only".
    headers.emplace_back("rbx-user-agent", "Roblox/Darwin");
    headers.emplace_back("Origin", "https://play.roblox.com");
    headers.emplace_back("Referer", "https://play.roblox.com/");
  }

  for (u32 pair = read_u32(attrs + kAttrHeaders); pair;
       pair = read_u32(pair + 4)) {
    const std::string name = ::weblox::guest::string(read_u32(pair));
    if (name.empty()) break;
    const u32 value_ptr = read_u32(pair + 4);
    const std::string value = value_ptr ? ::weblox::guest::string(value_ptr) : "";
    if (join && iequals(name, "rbx-user-agent"))
      continue;  // the native identity must win for joins
    if (roblox && iequals(name, "cookie"))
      continue;  // the injected session cookie must win
    headers.emplace_back(name, value);
  }
  // CURLOPT_USERAGENT above supplies the browser default when the guest did
  // not add its own User-Agent header.

  std::string body;
  const u32 body_ptr = read_u32(attrs + kAttrBodyPtr);
  const u32 body_len = read_u32(attrs + kAttrBodyLen);
  if (body_ptr && body_len && body_len <= kMaxBody) {
    if (const auto* bytes = ::weblox::guest::ptr(body_ptr, body_len))
      body.assign(reinterpret_cast<const char*>(bytes), body_len);
  }

  ::weblox::http::Response reply = ::weblox::http::request(
      method, url, headers, body, kMaxBody, kTimeoutMs);
  if (reply.status >= 200 && reply.status < 300 && join) {
    std::string fqdn;
    std::string patched = patch_join(reply.body, fqdn);
    if (patched != reply.body) {
      reply.body = std::move(patched);
      std::fprintf(stderr, "[http] injected gameFqdn %s\n", fqdn.c_str());
    }
  }
  set_headers(fetch, std::move(reply.headers));

  u32 data = 0;
  if (!reply.body.empty() &&
      reply.body.size() <= static_cast<std::size_t>(0xFFFFFFFFu))
    data = w2c_WebClient_malloc(g_wc_instance,
                                static_cast<u32>(reply.body.size()));
  if (data) {
    if (auto* dst = ::weblox::guest::ptr(data, reply.body.size()))
      std::memcpy(dst, reply.body.data(), reply.body.size());
  }
  ::weblox::guest::put_u32(fetch + kFetchDataPtr, data);
  ::weblox::guest::put_u64(fetch + kFetchNumBytes, data ? reply.body.size() : 0);
  ::weblox::guest::put_u64(fetch + kFetchDataOffset, 0);
  ::weblox::guest::put_u64(fetch + kFetchTotalBytes, data ? reply.body.size() : 0);
  write_u16(fetch + kFetchReadyState, 4);  // readyState DONE
  write_u16(fetch + kFetchStatus,
            static_cast<u16>(reply.status <= 0xFFFF ? reply.status : 0));
  const char* state = reply.status == 200 ? "OK"
                     : reply.status       ? "Error"
                                          : "NetworkError";
  write_text(fetch + kFetchStatusText, kFetchStatusTextCap, state);
  std::fprintf(stderr, "[http] -> %ld %zu bytes\n", reply.status,
               reply.body.size());

  call_callback(fetch, on_ready);
  if (reply.status >= 200 && reply.status < 300)
    call_callback(fetch, on_success);
  else
    call_callback(fetch, on_error);
}

void w2c_env_emscripten_fetch_free(struct w2c_env*, u32 fetch) {
  set_headers(fetch, {});
}

u32 w2c_env_0x5Femscripten_fetch_get_response_headers_length(struct w2c_env*,
                                                             u32 fetch) {
  const std::string headers = get_headers(fetch);
  return headers.size() > 0xFFFFFFFFu ? 0 : static_cast<u32>(headers.size());
}

u32 w2c_env_0x5Femscripten_fetch_get_response_headers(struct w2c_env*, u32 fetch,
                                                      u32 dst, u32 capacity) {
  const std::string headers = get_headers(fetch);
  const std::size_t n = std::min<std::size_t>(headers.size(), capacity);
  if (n) {
    if (auto* p = ::weblox::guest::ptr(dst, n)) std::memcpy(p, headers.data(), n);
  }
  return static_cast<u32>(n);
}

// The trivial __syscall_get*32 imports live with js_fetch_file in
// fetch_curl.cpp (they came from fetch.c in the reference host).

}  // extern "C"
