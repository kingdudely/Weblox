// Local content tree used by js_fetch_file (ported from v3-wasm2c/fetch.c and
// fs.c).  Guest URLs are resolved below a single root, never as host paths:
// `rbxasset://Foo/Bar.png` -> <WC_CONTENT>/Foo/Bar.png.  Remote HTTP URLs get a
// stable, sanitised file name under <WC_CONTENT>/http so a second run is local.
//
// This translation unit only deals with host files.  Guest memory is written
// by fetch_curl.cpp through weblox::guest helpers.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace weblox::fscache {

namespace fs = std::filesystem;

constexpr std::size_t kMaxFile = 256u << 20;  // same cap as fetch.c

enum class ReadResult { Ok, Missing, Invalid };

std::string content_root() {
  const char* value = std::getenv("WC_CONTENT");
  return (value && *value) ? std::string(value) : std::string("data/content");
}

bool safe_relative(const std::string& rel) {
  if (rel.empty() || rel.find("..") != std::string::npos) return false;
  // A colon would make the path absolute on Windows; a NUL cannot appear in
  // either a guest C string or a host path.
  if (rel.find(':') != std::string::npos ||
      rel.find('\0') != std::string::npos)
    return false;
  return !fs::path(rel).is_absolute();
}

bool resolve_local(const std::string& url, std::string& out) {
  std::size_t at = 0;
  if (url.compare(0, 11, "rbxasset://") == 0) at = 11;
  while (at < url.size() && url[at] == '/') ++at;
  std::string rel = url.substr(at);
  if (!safe_relative(rel)) return false;
  out = (fs::path(content_root()) / rel).string();
  return true;
}

bool resolve_remote(const std::string& url, std::string& out) {
  if (url.find("..") != std::string::npos) return false;
  // Keep a readable but portable name: anything outside [A-Za-z0-9._-]
  // becomes '_', and very long query strings are clipped.
  std::string name;
  name.reserve(url.size());
  for (char c : url) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                    (u >= '0' && u <= '9') || c == '.' || c == '_' || c == '-';
    name.push_back(ok ? c : '_');
    if (name.size() >= 180) break;
  }
  if (name.empty() || name == "." || name == "..") return false;
  out = (fs::path(content_root()) / "http" / name).string();
  return true;
}

ReadResult read_file(const std::string& path, std::string& out) {
  std::error_code ec;
  if (!fs::exists(fs::path(path), ec)) return ReadResult::Missing;
  std::uintmax_t size = fs::file_size(fs::path(path), ec);
  if (ec || size > kMaxFile) return ReadResult::Invalid;
  std::ifstream in(path, std::ios::binary);
  if (!in) return ReadResult::Invalid;
  out.assign(static_cast<std::size_t>(size), '\0');
  if (size && !in.read(&out[0], static_cast<std::streamsize>(size)))
    return ReadResult::Invalid;
  return ReadResult::Ok;
}

bool write_file(const std::string& path, const char* data, std::size_t len) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  if (len && !out.write(data, static_cast<std::streamsize>(len))) return false;
  return true;
}

}  // namespace weblox::fscache
