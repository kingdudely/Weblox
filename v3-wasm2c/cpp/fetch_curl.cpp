// js_fetch_file (ported from v3-wasm2c/fetch.c).  The engine asks the browser
// to read an rbxasset or CDN URL into guest memory; we serve mirrored assets
// from WC_CONTENT and fill misses through libcurl, caching them for next run.
//
// Declarations below are the two internal C++ APIs this file consumes.  They
// live in http_fetch.cpp and fs_cache.cpp; file-ownership rules for this port
// do not allow a shared header, so the signatures are repeated verbatim.
//
//   weblox::http::request  - one synchronous curl request with a byte cap
//   weblox::fscache::*     - local asset tree lookup / cache write

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace weblox::http {

struct Response {
  long status = 0;       // 0 means transport failure (XHR's NetworkError)
  std::string body;
  std::string headers;   // raw header block, as XHR's getAllResponseHeaders
};

Response request(const std::string& method, const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body, std::size_t max_bytes,
                 std::uint32_t timeout_ms);

}  // namespace weblox::http

namespace weblox::fscache {

enum class ReadResult { Ok, Missing, Invalid };

bool resolve_local(const std::string& url, std::string& out);
bool resolve_remote(const std::string& url, std::string& out);
ReadResult read_file(const std::string& path, std::string& out);
bool write_file(const std::string& path, const char* data, std::size_t len);

}  // namespace weblox::fscache

namespace {

constexpr std::size_t kMaxFetch = 256u << 20;
constexpr std::uint32_t kTimeoutMs = 120000;

bool verbose() {
  const char* value = std::getenv("WC_VERBOSE");
  return value && *value && std::strcmp(value, "0") != 0;
}

bool starts_with(const std::string& s, const char* prefix) {
  return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// Browser XHR cannot answer data:/rbxthumb URLs here; report a hard miss so
// the engine falls back to its own placeholder texture.
bool unsupported(const std::string& url) {
  return starts_with(url, "data:") || starts_with(url, "rbxthumb://");
}

bool copy_to_guest(u32 dst, const std::string& bytes) {
  if (auto* p = weblox::guest::ptr(dst, bytes.size())) {
    if (!bytes.empty()) std::memcpy(p, bytes.data(), bytes.size());
    return true;
  }
  return false;
}

// Download into the mirror.  `want` is the engine's expected size: a mismatch
// is a miss, not a partial success (the C host behaved the same way).
int fetch_remote(const std::string& url, const std::string& cache, u32 dst,
                 u32 want) {
  const auto reply = weblox::http::request("GET", url, {}, {}, kMaxFetch,
                                           kTimeoutMs);
  const bool ok = reply.status >= 200 && reply.status < 300 &&
                  !reply.body.empty() && reply.body.size() <= kMaxFetch &&
                  (!want || reply.body.size() == want);
  if (!ok || !copy_to_guest(dst, reply.body)) {
    if (verbose())
      std::fprintf(stderr, "[fetch] %s -> failed status=%ld want=%u\n",
                   url.c_str(), reply.status, want);
    return -1;
  }
  weblox::fscache::write_file(cache, reply.body.data(), reply.body.size());
  if (verbose())
    std::fprintf(stderr, "[fetch] %s -> %zu bytes (cached)\n", url.c_str(),
                 reply.body.size());
  return 0;
}

// Strip the scheme prefixes the C host accepted before forming the CDN URL.
std::string cdn_path(std::string url) {
  while (!url.empty() && url.front() == '/') url.erase(url.begin());
  if (starts_with(url, "rbxasset://")) url.erase(0, 11);
  while (!url.empty() && url.front() == '/') url.erase(url.begin());
  return url;
}

}  // namespace

extern "C" {

u32 w2c_env_js_fetch_file(struct w2c_env*, u32 url_ptr, u32 dst, u32 want) {
  const std::string url = weblox::guest::string(url_ptr);
  if (verbose())
    std::fprintf(stderr, "[fetch] %s want=%u\n",
                 url.empty() ? "(null)" : url.c_str(), want);
  if (url.empty() || unsupported(url)) return static_cast<u32>(-1);

  const bool is_http = starts_with(url, "http://") || starts_with(url, "https://");
  std::string path;
  if (!is_http) {
    if (!weblox::fscache::resolve_local(url, path)) {
      if (verbose())
        std::fprintf(stderr, "[fetch] %s -> noresolve\n", url.c_str());
      return static_cast<u32>(-2);
    }
    std::string bytes;
    const auto result = weblox::fscache::read_file(path, bytes);
    if (result == weblox::fscache::ReadResult::Ok) {
      if (bytes.size() > kMaxFetch || (want && bytes.size() != want) ||
          !copy_to_guest(dst, bytes))
        return static_cast<u32>(-1);
      if (verbose())
        std::fprintf(stderr, "[fetch] %s -> %zu bytes (local)\n", url.c_str(),
                     bytes.size());
      return 0;
    }
    if (result == weblox::fscache::ReadResult::Invalid)
      return static_cast<u32>(-1);
    // Not mirrored yet: try the CDN root configured by WC_BASE.
    const char* base = std::getenv("WC_BASE");
    if (!base || !*base) return static_cast<u32>(-2);
    const std::string remote = std::string(base) + cdn_path(url);
    if (!weblox::fscache::resolve_remote(remote, path))
      return static_cast<u32>(-1);
    return static_cast<u32>(fetch_remote(remote, path, dst, want));
  }

  if (!weblox::fscache::resolve_remote(url, path))
    return static_cast<u32>(-1);
  return static_cast<u32>(fetch_remote(url, path, dst, want));
}

// Emscripten's musl syscall stubs: the browser reports a single unprivileged
// user and group, and the engine only checks that the calls succeed.
u32 w2c_env_0x5F_syscall_getegid32(struct w2c_env*) { return 0; }
u32 w2c_env_0x5F_syscall_geteuid32(struct w2c_env*) { return 0; }
u32 w2c_env_0x5F_syscall_getgid32(struct w2c_env*) { return 0; }
u32 w2c_env_0x5F_syscall_getuid32(struct w2c_env*) { return 0; }

}  // extern "C"
