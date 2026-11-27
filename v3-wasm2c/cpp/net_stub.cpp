#include "net.h"

#ifndef WEBLOX_USE_CURL

namespace weblox::net {

Response request(const std::string&, const std::string&,
                 const std::vector<std::pair<std::string, std::string>>&,
                 const std::string&, std::uint32_t) {
  return {};
}

}  // namespace weblox::net

#endif  // !WEBLOX_USE_CURL
