// Port of the browser host's prepare() sequence. See host/PREPARE.md.
#include "prepare.h"

#include "imports.h"
#include "table.h"
#include "vmctx.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

extern "C" {
// Guest exports (wasm indices 372, 373, 385, 387). The linked object names
// defined function N - 341 (the 341 imports come first), so:
//   372 - 341 = 31  rbx_fs_set_base_url(path)
//   373 - 341 = 32  rbx_fs_register_asset(path, declared_size) -> i32
//   385 - 341 = 44  rbx_fs_preload_asset(path, data, len) -> i32
//   387 - 341 = 46  rbx_fs_add_file(path, data, len) -> i32
void _wasm_function_31(void*, void*, int32_t);
int32_t _wasm_function_32(void*, void*, int32_t, int32_t);
int32_t _wasm_function_44(void*, void*, int32_t, int32_t, int32_t);
int32_t _wasm_function_46(void*, void*, int32_t, int32_t, int32_t);

// Defined in rna_bridge.cpp: Wasmtime-50 wasm-call ABI shim.  It loads each
// entry of `args` as a 64-bit register value, so a uint64_t slot can carry an
// embind i64 argument (set_place_id) as well as an i32/pointer.
int64_t wb_rna_invoke(void*, void*, void*, const uint32_t*, uint32_t);
}

namespace weblox {
namespace {

constexpr const char* kWebBase = "https://www.roblox.com/";
constexpr const char* kAssetBase =
    "https://setup.rbxcdn.com/webclient/version-1ef59424f70d4932-RobloxWebClient/";

// Mirrors v2-wasmtime/src/main.rs::settings()'s fallback (MIN_RAK). The page
// patches the fetched settings object by assigning the two flags last, so
// duplicate keys resolve to False in the engine's last-wins JSON parser.
constexpr const char* kSettings =
    "{\"applicationSettings\":{\"BaseUrl\":\"https://www.roblox.com/\","
    "\"FIntWasmMemoryPages\":\"32768\","
    "\"FFlagUseRbxTransportClient3\":\"False\","
    "\"FFlagUseRbxTransportServer\":\"False\"},"
    "\"RobloxLocaleId\":\"en-us\",\"GameLocaleId\":\"en-us\"}";

constexpr const char* kAppStorage =
    "{\"WebLogin\":\"2\",\"RobloxLocaleId\":\"en-us\",\"GameLocaleId\":\"en-us\"}";

bool trace() { return std::getenv("HOST_TRACE") != nullptr; }

void st32(host::HostState& s, uint32_t addr, uint32_t value) {
  std::memcpy(s.memory_base + addr, &value, 4);
}

bool mem_write(host::HostState& s, uint32_t addr, const void* src, size_t len) {
  if (!s.memory_base || addr > s.memory_def_storage.current_length ||
      len > s.memory_def_storage.current_length - addr) {
    return false;
  }
  std::memcpy(s.memory_base + addr, src, len);
  return true;
}

// Guest malloc export (_wasm_function_149984 == wasm index 150325).
uint32_t guest_malloc(host::HostState& s, uint32_t size) {
  const uint32_t p = _wasm_function_149984(s.vmctx, s.vmctx, size);
  if (!p || static_cast<uint64_t>(p) + size > s.memory_def_storage.current_length) {
    std::fprintf(stderr, "[prep] guest malloc(%u) failed: %u\n", size, p);
    return 0;
  }
  return p;
}

// C string in guest memory (NUL terminated), for the rbx_fs_* exports.
uint32_t import_bytes(host::HostState& s, const std::string& bytes) {
  const uint32_t p = guest_malloc(s, static_cast<uint32_t>(bytes.size() + 1));
  if (!p) return 0;
  if (!mem_write(s, p, bytes.data(), bytes.size())) return 0;
  mem_write(s, p + static_cast<uint32_t>(bytes.size()), "\0", 1);
  return p;
}

// embind std::string wire form: u32 length followed by the bytes. `nul`
// selects binds::guest_string_bytes() (length+5); without it the layout is
// binds::guest_string() (length+4).
uint32_t guest_string(host::HostState& s, const std::string& text, bool nul) {
  const uint32_t len = static_cast<uint32_t>(text.size());
  const uint32_t p = guest_malloc(s, len + 4 + (nul ? 1u : 0u));
  if (!p) return 0;
  if (!mem_write(s, p, &len, 4)) return 0;
  if (!text.empty() && !mem_write(s, p + 4, text.data(), text.size())) return 0;
  if (nul) mem_write(s, p + 4 + len, "\0", 1);
  return p;
}

// Call an embind binding through its registered invoker in the indirect table.
// args[0..n-1] are the binding's arguments; arg 0 of the wasm call is always
// the binding id (the receiver placeholder recorded by _embind_register_function).
bool invoke_binding(host::HostState& s, const char* name,
                    const uint64_t* args, uint32_t n) {
  const wb::Binding* b = wb::embind_find(name);
  if (!b) {
    std::fprintf(stderr, "[prep] MISSING embind binding %s\n", name);
    return false;
  }
  if (b->invoker >= wb_table_size || !s.table_funcrefs ||
      !s.table_funcrefs[b->invoker].wasm_call) {
    std::fprintf(stderr, "[prep] binding %s has no invoker at table %u\n",
                 name, b->invoker);
    return false;
  }
  if (trace() || b->argc != n) {
    std::fprintf(stderr,
                 "[prep] %s id=%u invoker=%u sig=%s argc=%u n=%u\n",
                 name, b->id, b->invoker, b->sig, b->argc, n);
  }
  uint64_t a[6] = {0, 0, 0, 0, 0, 0};
  a[0] = b->id;
  for (uint32_t i = 0; i < n && i < 5; ++i) a[i + 1] = args[i];
  wb_rna_invoke(s.vmctx, s.vmctx,
                s.table_funcrefs[b->invoker].wasm_call,
                reinterpret_cast<const uint32_t*>(a), n + 1);
  return true;
}

// Convenience wrappers for the three string setter shapes used by prepare().
bool set_string(host::HostState& s, const char* name, const std::string& value) {
  const uint32_t p = guest_string(s, value, /*nul=*/false);
  if (!p) return false;
  const uint64_t args[1] = {p};
  return invoke_binding(s, name, args, 1);
}

bool set_two_strings(host::HostState& s, const char* name,
                     const std::string& a, const std::string& b) {
  const uint32_t pa = guest_string(s, a, /*nul=*/false);
  const uint32_t pb = guest_string(s, b, /*nul=*/false);
  if (!pa || !pb) return false;
  const uint64_t args[2] = {pa, pb};
  return invoke_binding(s, name, args, 2);
}

// --- meta.json parsing (same shape as v2-wasmtime/src/main.rs::field) ------

std::string json_string_field(const std::string& chunk, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const std::size_t i = chunk.find(key);
  if (i == std::string::npos) return {};
  std::size_t p = i + key.size();
  while (p < chunk.size() && (chunk[p] == ' ' || chunk[p] == '\t')) ++p;
  if (p >= chunk.size() || chunk[p] != '"') return {};
  const std::size_t end = chunk.find('"', p + 1);
  return end == std::string::npos ? std::string{}
                                  : chunk.substr(p + 1, end - p - 1);
}

uint32_t json_uint_field(const std::string& chunk, const char* name,
                         uint32_t fallback) {
  const std::string key = std::string("\"") + name + "\":";
  const std::size_t i = chunk.find(key);
  if (i == std::string::npos) return fallback;
  std::size_t p = i + key.size();
  while (p < chunk.size() && (chunk[p] == ' ' || chunk[p] == '\t')) ++p;
  uint64_t value = 0;
  while (p < chunk.size() && chunk[p] >= '0' && chunk[p] <= '9') {
    value = value * 10 + static_cast<unsigned>(chunk[p] - '0');
    ++p;
  }
  return static_cast<uint32_t>(value);
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

std::string resolve_meta(const std::string& requested) {
  std::error_code ec;
  const char* candidates[] = {
      requested.c_str(),
      "data/meta.json",
      "../v2-wasmtime/data/meta.json",
      "v2-wasmtime/data/meta.json",
  };
  for (const char* c : candidates) {
    if (c && *c && std::filesystem::is_regular_file(c, ec)) return c;
  }
  return {};
}

// Find a file whose path ends with `tail`, recursively, bounded. The content
// mirror is gitignored and usually absent; absence is not an error.
std::string find_under(const std::string& root, const std::string& tail) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return {};
  unsigned seen = 0;
  auto it = std::filesystem::recursive_directory_iterator(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  auto end = std::filesystem::recursive_directory_iterator();
  for (; !ec && it != end && seen < 200000; it.increment(ec), ++seen) {
    const auto& p = it->path();
    if (p.filename().empty()) continue;
    std::string s = p.string();
    if (s.size() >= tail.size() &&
        s.compare(s.size() - tail.size(), tail.size(), tail) == 0) {
      return s;
    }
  }
  return {};
}

}  // namespace

uint32_t guest_argv(host::HostState& s) {
  // Same allocation order as the Rust port: string first, then the 2-word
  // array. malloc() is an exported wasm function with the Wasmtime-50 ABI.
  const uint32_t str = guest_malloc(s, 16);
  const uint32_t arr = guest_malloc(s, 8);
  if (!str || !arr) return 0;
  static const char kArgv0[] = "/content/.keep";
  if (!mem_write(s, str, kArgv0, sizeof(kArgv0))) return 0;
  st32(s, arr, str);
  st32(s, arr + 4, 0);
  return arr;
}

bool prepare(host::HostState& s, uint64_t place_id, const std::string& cookie,
             const std::string& meta_path, const std::string& content_base) {
  (void)cookie;  // route fetch in main.cpp already used it; never log it.
  if (!s.vmctx || !s.memory_base || !wb::imports_has_memory()) {
    std::fprintf(stderr, "[prep] no guest memory/vmctx\n");
    return false;
  }
  bool ok = true;
  const char* root = std::getenv("WC_CONTENT");
  const std::string content = (root && *root) ? root : content_base;

  // ---- 1. embind settings (order matches v2-wasmtime/src/main.rs::prepare) --
  ok &= set_string(s, "set_pretend_platform", "desktop");
  ok &= set_string(s, "set_roblox_base_url", kWebBase);
  ok &= set_string(s, "set_channel", "");
  ok &= set_string(s, "set_browser_tracker_id", "");
  ok &= set_two_strings(s, "set_account_locales", "en-us", "en-us");

  const uint32_t flags = guest_string(s, kSettings, /*nul=*/true);
  if (!flags) {
    ok = false;
  } else {
    const uint64_t args[1] = {flags};
    ok &= invoke_binding(s, "set_preloaded_flags", args, 1);
  }
  // set_place_id takes an embind i64: the shim loads this slot as one 64-bit
  // wasm argument (rcx), not two i32 words.
  const uint64_t place_arg[1] = {place_id};
  ok &= invoke_binding(s, "set_place_id", place_arg, 1);

  // ---- 2. VFS: declared assets from meta.json ------------------------------
  const std::string meta_file = resolve_meta(meta_path);
  uint32_t registered = 0, preloaded = 0, files = 0;
  if (meta_file.empty()) {
    std::fprintf(stderr, "[prep] meta.json not found (tried %s)\n",
                 meta_path.c_str());
    ok = false;
  } else {
    std::string text;
    if (!read_file(meta_file, text)) {
      std::fprintf(stderr, "[prep] cannot read %s\n", meta_file.c_str());
      ok = false;
    } else {
      std::size_t pos = 0;
      while ((pos = text.find('{', pos)) != std::string::npos) {
        const std::size_t end = text.find('}', pos);
        if (end == std::string::npos) break;
        const std::string entry = text.substr(pos, end - pos);
        pos = end + 1;
        const std::string path = json_string_field(entry, "path");
        if (path.empty()) continue;
        const uint32_t size = json_uint_field(entry, "size", 1);
        const uint32_t p = import_bytes(s, path);
        if (!p) {
          ok = false;
          break;
        }
        _wasm_function_32(s.vmctx, s.vmctx, static_cast<int32_t>(p),
                          static_cast<int32_t>(size));
        ++registered;
      }
    }
  }
  std::fprintf(stderr, "[prep] registered %u asset paths (meta=%s)\n",
               registered, meta_file.empty() ? "none" : meta_file.c_str());

  // ---- 3. InExperience assets, if the (gitignored) mirror is present -------
  static const char* kPreload[] = {
      "/models/InExperience/InExperience.rbxm",
      "/models/InExperience/InExperience_checksum",
  };
  for (const char* rel : kPreload) {
    const std::string tail = rel + 1;  // trim leading '/'
    const std::string disk = find_under(content, tail);
    if (disk.empty()) {
      if (trace()) std::fprintf(stderr, "[prep] no mirror file for %s\n", rel);
      continue;
    }
    std::string body;
    if (!read_file(disk, body)) continue;
    const uint32_t path = import_bytes(s, rel);
    const uint32_t data = import_bytes(s, body);
    if (!path || !data) {
      ok = false;
      continue;
    }
    const auto len = static_cast<int32_t>(body.size());
    _wasm_function_32(s.vmctx, s.vmctx, static_cast<int32_t>(path), len);
    const int32_t rc = _wasm_function_44(
        s.vmctx, s.vmctx, static_cast<int32_t>(path),
        static_cast<int32_t>(data), len);
    ++preloaded;
    std::fprintf(stderr, "[prep] preload %s (%zu bytes) -> %d\n", rel,
                 body.size(), rc);
  }

  // ---- 4. appStorage.json and the asset base URL ---------------------------
  const uint32_t app = import_bytes(s, kAppStorage);
  const uint32_t app_path =
      import_bytes(s, "/appData/LocalStorage/appStorage.json");
  if (app && app_path) {
    // Rust port hardcodes 61 here, which truncates the 64-byte JSON. Use the
    // real length so the engine sees a valid appStorage file.
    _wasm_function_46(s.vmctx, s.vmctx, static_cast<int32_t>(app_path),
                      static_cast<int32_t>(app),
                      static_cast<int32_t>(std::strlen(kAppStorage)));
    ++files;
  } else {
    ok = false;
  }
  const uint32_t base = import_bytes(s, kAssetBase);
  if (base) {
    _wasm_function_31(s.vmctx, s.vmctx, static_cast<int32_t>(base));
  } else {
    ok = false;
  }

  std::fprintf(stderr,
               "[prep] done: ok=%d place=%llu assets=%u preload=%u files=%u "
               "base=%s\n",
               ok ? 1 : 0, static_cast<unsigned long long>(place_id),
               registered, preloaded, files, kAssetBase);
  return ok;
}

}  // namespace weblox
