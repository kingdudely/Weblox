// VFS / asset bootstrap, ported from v2-wasmtime/src/main.rs::prepare()
// (lines 792-833).  The engine exposes four exports for this in WC.h:
//
//   u32  rbx_fs_register_asset(w2c_WebClient*, u32 path, u32 size)
//   u32  rbx_fs_preload_asset (w2c_WebClient*, u32 path, u32 body, u32 len)
//   u32  rbx_fs_add_file      (w2c_WebClient*, u32 path, u32 body, u32 len)
//   void rbx_fs_set_base_url  (w2c_WebClient*, u32 url)
//
// All guest buffers come from w2c_WebClient_malloc and are freed after the
// call.  No import is needed: js_fetch_file (the only related import used by
// the join path) lives in fetch_curl.cpp, and the rbx_opfs_* imports are not
// touched by this bootstrap.

#include "cpp_compat.h"
extern "C" {
#include "WC.h"
}

#include "guest.h"
#include "vfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace weblox::vfs {

namespace fs = std::filesystem;

namespace {

constexpr const char* kAssetBase =
    "https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient/";
constexpr const char* kAppStorage =
    R"({"WebLogin":"2","RobloxLocaleId":"en-us","GameLocaleId":"en-us"})";
constexpr const char* kPreload[] = {
    "/models/InExperience/InExperience.rbxm",
    "/models/InExperience/InExperience_checksum",
};
constexpr std::size_t kMaxAsset = 256u << 20;  // matches fetch_curl.cpp

// A wasm exception escaping an export would longjmp through the host frame;
// install the same unwind target loop.cpp uses for main_loop_iteration.
template <class F>
bool guarded(F&& fn) {
  WASM_RT_UNWIND_TARGET* outer = wasm_rt_get_unwind_target();
  WASM_RT_UNWIND_TARGET target;
  if (wasm_rt_try(target)) {
    wasm_rt_set_unwind_target(outer);
    std::fprintf(stderr, "[vfs] wasm exception tag=%p bytes=%u\n",
                 const_cast<void*>(wasm_rt_exception_tag()),
                 wasm_rt_exception_size());
    return false;
  }
  wasm_rt_set_unwind_target(&target);
  fn();
  wasm_rt_set_unwind_target(outer);
  return true;
}

// Allocate a NUL-terminated guest buffer (Rust's import_bytes()).  Binary
// bodies get the same trailing NUL; the engine is told the real length.
u32 put_guest(w2c_WebClient* inst, const void* bytes, std::size_t len) {
  if (len >= std::numeric_limits<u32>::max()) return 0;
  const u32 address = w2c_WebClient_malloc(inst, static_cast<u32>(len + 1));
  if (!address) return 0;
  auto* dst = weblox::guest::ptr(address, len + 1);
  if (!dst) {
    w2c_WebClient_free(inst, address);
    return 0;
  }
  if (len) std::memcpy(dst, bytes, len);
  dst[len] = 0;
  return address;
}

void drop_guest(w2c_WebClient* inst, u32 address) {
  if (address) w2c_WebClient_free(inst, address);
}

bool register_asset(w2c_WebClient* inst, const std::string& path, u32 size) {
  const u32 p = put_guest(inst, path.data(), path.size());
  if (!p) {
    std::fprintf(stderr, "[vfs] register allocation failed: %s\n",
                 path.c_str());
    return false;
  }
  u32 rc = 0;
  const bool ok = guarded([&] {
    rc = w2c_WebClient_rbx_fs_register_asset(inst, p, size);
  });
  drop_guest(inst, p);
  // The Rust host ignores the return value; its meaning is version-dependent.
  // Only host-side allocation failures and wasm exceptions are fatal here.
  (void)rc;
  if (!ok)
    std::fprintf(stderr, "[vfs] register threw: %s (size=%u)\n", path.c_str(),
                 size);
  return ok;
}

bool preload_asset(w2c_WebClient* inst, const char* path,
                   const std::string& body) {
  if (body.size() > kMaxAsset) {
    std::fprintf(stderr, "[vfs] preload %s too large (%zu bytes)\n", path,
                 body.size());
    return false;
  }
  const u32 p = put_guest(inst, path, std::strlen(path));
  const u32 d = put_guest(inst, body.data(), body.size());
  if (!p || !d) {
    drop_guest(inst, p);
    drop_guest(inst, d);
    std::fprintf(stderr, "[vfs] preload allocation failed: %s\n", path);
    return false;
  }
  const u32 len = static_cast<u32>(body.size());
  u32 rc = 0;
  const bool ok = guarded([&] {
    rc = w2c_WebClient_rbx_fs_preload_asset(inst, p, d, len);
  });
  drop_guest(inst, d);
  drop_guest(inst, p);
  std::fprintf(stderr, "[vfs] preload %s (%u bytes) -> %u%s\n", path, len, rc,
               ok ? "" : " (wasm exception)");
  return ok;
}

bool add_file(w2c_WebClient* inst, const char* path, const char* body) {
  const u32 p = put_guest(inst, path, std::strlen(path));
  const u32 d = put_guest(inst, body, std::strlen(body));
  if (!p || !d) {
    drop_guest(inst, p);
    drop_guest(inst, d);
    std::fprintf(stderr, "[vfs] add_file allocation failed: %s\n", path);
    return false;
  }
  const u32 len = static_cast<u32>(std::strlen(body));
  u32 rc = 0;
  const bool ok = guarded([&] {
    rc = w2c_WebClient_rbx_fs_add_file(inst, p, d, len);
  });
  drop_guest(inst, d);
  drop_guest(inst, p);
  std::fprintf(stderr, "[vfs] add_file %s (%u bytes) -> %u%s\n", path, len, rc,
               ok ? "" : " (wasm exception)");
  return ok;
}

bool set_base_url(w2c_WebClient* inst, const char* url) {
  const u32 p = put_guest(inst, url, std::strlen(url));
  if (!p) {
    std::fprintf(stderr, "[vfs] base URL allocation failed\n");
    return false;
  }
  const bool ok = guarded([&] { w2c_WebClient_rbx_fs_set_base_url(inst, p); });
  drop_guest(inst, p);
  return ok;
}

// Minimal JSON reader for the flat {"path": "...", "size": N} objects in
// meta.json.  Mirrors v2-wasmtime's field(): first "key":, quoted value or
// leading digits.  Common escapes are decoded; \uXXXX is out of scope.
bool ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string json_field(const std::string& object, const char* name) {
  std::string key = "\"";
  key += name;
  key += "\":";
  const std::size_t at = object.find(key);
  if (at == std::string::npos) return {};
  std::size_t i = at + key.size();
  while (i < object.size() && ascii_space(object[i])) ++i;
  if (i >= object.size()) return {};
  if (object[i] == '"') {
    ++i;
    std::string out;
    while (i < object.size() && object[i] != '"') {
      char c = object[i++];
      if (c == '\\' && i < object.size()) {
        switch (object[i++]) {
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 't': c = '\t'; break;
          case '"': c = '"'; break;
          case '\\': c = '\\'; break;
          case '/': c = '/'; break;
          default: break;  // \uXXXX is not used in the manifest
        }
      }
      out.push_back(c);
    }
    return out;
  }
  std::string out;
  while (i < object.size() && object[i] >= '0' && object[i] <= '9')
    out.push_back(object[i++]);
  return out;
}

u32 declared_size(const std::string& text) {
  unsigned long long value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') break;
    value = value * 10u + static_cast<unsigned>(c - '0');
    if (value > std::numeric_limits<u32>::max())
      return 1;  // Rust's parse::<u32>() would fail -> unwrap_or(1)
  }
  return value ? static_cast<u32>(value) : 1;  // Rust's parse().unwrap_or(1)
}

bool load_file(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>());
  return !in.bad();
}

bool load_meta(std::string& text, std::string& used) {
  std::vector<std::string> candidates;
  if (const char* custom = std::getenv("WC_META"); custom && *custom)
    candidates.emplace_back(custom);
  candidates.emplace_back("data/meta.json");
  candidates.emplace_back("v3-wasm2c/data/meta.json");
  for (const auto& candidate : candidates) {
    if (load_file(fs::path(candidate), text)) {
      used = candidate;
      return true;
    }
  }
  return false;
}

// Locate a mirrored asset by exact relative path first, then by path suffix,
// as v2-wasmtime::find_under() does (the mirror keeps the engine's layout).
std::string content_root() {
  const char* value = std::getenv("WC_CONTENT");
  return (value && *value) ? std::string(value) : std::string("data/content");
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return suffix.size() <= value.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

fs::path find_under(const fs::path& root, const std::string& tail) {
  std::error_code ec;
  const fs::path direct = root / fs::path(tail);
  if (fs::is_regular_file(direct, ec)) return direct;
  if (!fs::is_directory(root, ec)) return {};
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  for (; it != end && !ec; it.increment(ec)) {
    if (it->is_regular_file(ec) && ends_with(it->path().generic_string(), tail))
      return it->path();
  }
  return {};
}

}  // namespace

bool bootstrap(w2c_WebClient* inst) {
  if (!inst) {
    std::fprintf(stderr, "[vfs] bootstrap: null instance\n");
    return false;
  }

  std::string meta;
  std::string meta_path;
  std::size_t registered = 0;
  std::size_t malformed = 0;
  if (std::getenv("WC_NOREG")) {
    std::fprintf(stderr, "[vfs] WC_NOREG set; skipping meta registration\n");
  } else if (!load_meta(meta, meta_path)) {
    std::fprintf(stderr,
                 "[vfs] meta.json missing or unreadable (tried WC_META, "
                 "data/meta.json, v3-wasm2c/data/meta.json); continuing\n");
  } else {
    std::size_t pos = 0;
    while ((pos = meta.find('{', pos)) != std::string::npos) {
      ++pos;  // skip the opening brace, as Rust's split('{').skip(1) does
      const std::size_t stop = meta.find('}', pos);
      const std::string entry =
          meta.substr(pos, stop == std::string::npos ? std::string::npos
                                                     : stop - pos);
      const std::string path = json_field(entry, "path");
      if (path.empty()) {
        ++malformed;
      } else if (register_asset(inst, path, declared_size(json_field(entry, "size")))) {
        ++registered;
      }
      if (stop == std::string::npos) break;
      pos = stop + 1;
    }
    std::fprintf(stderr,
                 "[vfs] registered %zu asset paths (meta=%s, skipped=%zu)\n",
                 registered, meta_path.c_str(), malformed);
  }

  const std::string root = content_root();
  for (const char* rel : kPreload) {
    std::string tail(rel);
    while (!tail.empty() && tail.front() == '/') tail.erase(tail.begin());
    const fs::path disk = find_under(fs::path(root), tail);
    if (disk.empty()) {
      std::fprintf(stderr, "[vfs] no mirror file for %s under %s\n", rel,
                   root.c_str());
      continue;
    }
    std::string body;
    if (!load_file(disk, body) || body.empty()) {
      std::fprintf(stderr, "[vfs] unreadable mirror file for %s (%s)\n", rel,
                   disk.u8string().c_str());
      continue;
    }
    // The mirror holds the real byte length: re-register before preloading.
    register_asset(inst, rel, static_cast<u32>(body.size()));
    preload_asset(inst, rel, body);
  }

  // The Rust host passes 61 here, which truncates this 64-byte blob.  The C
  // host parity note (host/PREPARE.md) uses the full documented JSON instead.
  add_file(inst, "/appData/LocalStorage/appStorage.json", kAppStorage);

  const char* base = std::getenv("WC_ASSET_BASE");
  if (!base || !*base) base = kAssetBase;
  if (!set_base_url(inst, base)) {
    std::fprintf(stderr, "[vfs] bootstrap failed: base URL not set\n");
    return false;
  }
  std::fprintf(stderr, "[vfs] bootstrap ok: base=%s assets=%zu\n", base,
               registered);
  return true;
}

}  // namespace weblox::vfs
