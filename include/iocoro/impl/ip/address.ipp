#include <iocoro/ip/address.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// inet_pton / inet_ntop
#include <arpa/inet.h>
#include <netinet/in.h>

namespace iocoro::ip {

namespace {

// Convert a string_view to a null-terminated C string in a fixed-size buffer.
// Returns false if the input is too long to fit (including terminator).
template <std::size_t N>
inline auto to_cstr(std::string_view s, char (&buf)[N]) noexcept -> bool {
  static_assert(N > 0);
  if (s.size() >= N) {
    return false;
  }
  if (!s.empty()) {
    std::memcpy(buf, s.data(), s.size());
  }
  buf[s.size()] = '\0';
  return true;
}

}  // namespace

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

inline auto address_v4::from_string(std::string_view s) -> expected<address_v4, std::error_code> {
  auto addr = in_addr{};
  // inet_pton expects a null-terminated string.
  char buf[46]{};  // max textual IP length + null (IPv6 max is 45)
  if (!to_cstr(s, buf)) {
    return unexpected(error::invalid_argument);
  }
  if (::inet_pton(AF_INET, buf, &addr) != 1) {
    return unexpected(error::invalid_argument);
  }
  bytes_type b{};
  std::memcpy(b.data(), &addr.s_addr, 4);
  return address_v4{b};
}

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

inline auto address_v6::from_string(std::string_view s) -> expected<address_v6, std::error_code> {
  std::uint32_t scope = 0;
  auto ip_part = s;

  if (auto const pos = s.find('%'); pos != std::string_view::npos) {
    ip_part = s.substr(0, pos);
    auto scope_part = s.substr(pos + 1);
    if (scope_part.empty()) {
      return unexpected(error::invalid_argument);
    }
    auto const* first = scope_part.data();
    auto const* last = scope_part.data() + scope_part.size();
    auto r = std::from_chars(first, last, scope);
    if (r.ec != std::errc{} || r.ptr != last) {
      return unexpected(error::invalid_argument);
    }
  }

  auto addr = in6_addr{};
  char buf[46]{};  // max textual IPv6 length + null (without %scope)
  if (!to_cstr(ip_part, buf)) {
    return unexpected(error::invalid_argument);
  }
  if (::inet_pton(AF_INET6, buf, &addr) != 1) {
    return unexpected(error::invalid_argument);
  }

  bytes_type b{};
  std::memcpy(b.data(), addr.s6_addr, 16);
  return address_v6{b, scope};
}

}  // namespace iocoro::ip
