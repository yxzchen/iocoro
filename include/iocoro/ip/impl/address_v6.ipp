#include <iocoro/ip/address_v6.hpp>

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>

// inet_pton / inet_ntop
#include <arpa/inet.h>
#include <netinet/in.h>

namespace iocoro::ip {

inline auto address_v6::to_string() const -> std::string {
  auto addr = in6_addr{};
  auto const b = to_bytes();
  static_assert(sizeof(addr.s6_addr) == 16);
  std::memcpy(addr.s6_addr, b.data(), 16);

  char buf[INET6_ADDRSTRLEN]{};
  if (::inet_ntop(AF_INET6, &addr, buf, sizeof(buf)) == nullptr) {
    return "::";
  }

  std::string out(buf);
  if (scope_id_ != 0) {
    out.push_back('%');
    out += std::to_string(scope_id_);
  }
  return out;
}

inline auto address_v6::from_string(std::string const& s) -> result<address_v6> {
  std::uint32_t scope = 0;
  std::string ip_part;

  if (auto const pos = s.find('%'); pos != std::string::npos) {
    ip_part = s.substr(0, pos);
    auto scope_str = s.substr(pos + 1);
    if (scope_str.empty()) {
      return unexpected(error::invalid_argument);
    }
    auto const* first = scope_str.data();
    auto const* last = scope_str.data() + scope_str.size();
    auto r = std::from_chars(first, last, scope);
    if (r.ec != std::errc{} || r.ptr != last) {
      return unexpected(error::invalid_argument);
    }
  } else {
    ip_part = s;
  }

  auto addr = in6_addr{};
  if (::inet_pton(AF_INET6, ip_part.c_str(), &addr) != 1) {
    return unexpected(error::invalid_argument);
  }

  bytes_type b{};
  std::memcpy(b.data(), addr.s6_addr, 16);
  return address_v6{b, scope};
}

}  // namespace iocoro::ip
