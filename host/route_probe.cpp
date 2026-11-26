// Standalone check for the native join-route helper. No wasm object, no game
// client, no browser: just one gamejoin POST and the route derivation.
//
//   g++ -std=c++17 -O1 -Wall -Wextra -I. -o route_probe route_probe.cpp route.cpp -lcurl
//   timeout 60s ./route_probe 189707
//
// Defaults: place 189707, cookie from /home/john/mizg.txt (COOKIE_FILE=... to
// override). Everything on stdout is one JSON object; diagnostics go to stderr.
#include "route.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {

std::string trim(const std::string& s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                   s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

std::string quoted(const std::string& s) {
  std::string out = "\"";
  static const char* digits = "0123456789abcdef";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
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
  return out;
}

} // namespace

int main(int argc, char** argv) {
  unsigned long long place = 189707;
  std::string cookie_file = "/home/john/mizg.txt";
  bool show_script = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr,
                   "usage: route_probe [PLACE] [--cookie FILE] [--show-script]\n");
      return 0;
    }
    if (arg == "--cookie" && i + 1 < argc) {
      cookie_file = argv[++i];
    } else if (arg == "--show-script") {
      show_script = true;
    } else if (arg[0] != '-') {
      place = std::strtoull(arg.c_str(), nullptr, 10);
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return 2;
    }
  }
  if (const char* env = std::getenv("COOKIE_FILE")) cookie_file = env;

  std::ifstream file(cookie_file);
  if (!file) {
    std::fprintf(stderr, "[route-probe] cannot read cookie file %s\n", cookie_file.c_str());
    return 1;
  }
  std::ostringstream buf;
  buf << file.rdbuf();
  const std::string cookie = trim(buf.str());
  if (cookie.empty()) {
    std::fprintf(stderr, "[route-probe] empty cookie in %s\n", cookie_file.c_str());
    return 1;
  }

  weblox::FetchOptions options;
  options.max_attempts = 5;
  options.timeout_ms = 8000;     // 5 x 8 s + backoff < 45 s, bounded by the caller's timeout
  options.connect_timeout_ms = 5000;
  options.retry_delay_ms = 200;

  auto started = std::chrono::steady_clock::now();
  try {
    const weblox::JoinRoute route =
        weblox::fetch(static_cast<std::uint64_t>(place), cookie, options);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "[route-probe] place=%llu elapsed=%lldms status=%ld attempts<=%u\n",
                 place, static_cast<long long>(elapsed), route.status, options.max_attempts);

    std::cout << "{\n";
    std::cout << "  \"fqdn\": " << quoted(route.fqdn) << ",\n";
    std::cout << "  \"host\": " << quoted(route.host) << ",\n";
    std::cout << "  \"nativeHost\": " << quoted(route.native_host) << ",\n";
    std::cout << "  \"port\": " << route.port << ",\n";
    std::cout << "  \"caps\": " << quoted(route.caps) << ",\n";
    std::cout << "  \"certHashes\": [" << quoted(route.cert_hash) << "],\n";
    std::cout << "  \"pubKey\": " << quoted(route.pub_key) << ",\n";
    std::cout << "  \"gameId\": " << quoted(route.game_id) << ",\n";
    std::cout << "  \"placeId\": " << place << ",\n";
    std::cout << "  \"joinScriptLength\": " << route.join_script.size();
    if (show_script) {
      std::cout << ",\n  \"joinScript\": " << quoted(route.join_script);
    }
    std::cout << "\n}\n";
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[route-probe] ERROR: %s\n", e.what());
    return 1;
  }
}
