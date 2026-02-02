#include <iocoro/ip/address_v4.hpp>

#include <cstdint>
#include <cstring>
#include <string>

// inet_pton / inet_ntop
#include <arpa/inet.h>
#include <netinet/in.h>

namespace iocoro::ip {

inline auto address_v4::to_string() const -> std::string {
  auto addr = in_addr{};
  static_assert(sizeof(addr.s_addr) == 4);
  auto const b = to_bytes();
  std::memcpy(&addr.s_addr, b.data(), 4);

  char buf[INET_ADDRSTRLEN]{};
  if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) == nullptr) {
    // Best-effort: should never fail for AF_INET + 4 bytes.
    return "0.0.0.0";
  }
  return std::string(buf);
}

inline auto address_v4::from_string(std::string const& s) -> result<address_v4> {
  auto addr = in_addr{};
  if (::inet_pton(AF_INET, s.c_str(), &addr) != 1) {
    return unexpected(error::invalid_argument);
  }
  bytes_type b{};
  std::memcpy(b.data(), &addr.s_addr, 4);
  return address_v4{b};
}

}  // namespace iocoro::ip
