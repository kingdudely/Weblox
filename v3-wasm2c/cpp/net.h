// HTTP abstraction.  Default stub returns status 0; compile net_curl.cpp with
// -DWEBLOX_USE_CURL and link libcurl for real HTTP/1.1 + HTTP/2 requests.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace weblox::net {

struct Response {
  std::int64_t status = 0;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

Response request(const std::string& method, const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body, std::uint32_t timeout_ms);

}  // namespace weblox::net
