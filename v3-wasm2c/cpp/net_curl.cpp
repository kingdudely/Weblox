// HTTP via libcurl.  Enabled with -DWEBLOX_USE_CURL; without that define this
// translation unit is empty and net_stub.cpp supplies the no-op version.
#ifdef WEBLOX_USE_CURL

#include "net.h"

#include <curl/curl.h>

#include <string>

namespace weblox::net {
namespace {
std::size_t write_body(void* data, std::size_t size, std::size_t count,
                       void* user) {
  auto* body = static_cast<std::string*>(user);
  body->append(static_cast<const char*>(data), size * count);
  return size * count;
}
std::size_t write_header(char* data, std::size_t size, std::size_t count,
                         void* user) {
  auto* headers =
      static_cast<std::vector<std::pair<std::string, std::string>>*>(user);
  std::string line(data, size * count);
  auto colon = line.find(':');
  if (colon != std::string::npos) {
    headers->emplace_back(line.substr(0, colon),
                          line.substr(colon + 1));
  }
  return size * count;
}
}  // namespace

Response request(const std::string& method, const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body, std::uint32_t timeout_ms) {
  Response out;
  CURL* curl = curl_easy_init();
  if (!curl) return out;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  if (method != "GET") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
  }
  curl_slist* list = nullptr;
  for (const auto& [key, value] : headers) {
    list = curl_slist_append(list, (key + ": " + value).c_str());
  }
  if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

  out.status = curl_easy_perform(curl) == CURLE_OK ? 200 : 0;
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return out;
}

}  // namespace weblox::net

#endif  // WEBLOX_USE_CURL
