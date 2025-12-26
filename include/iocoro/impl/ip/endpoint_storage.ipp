#include <iocoro/ip/detail/endpoint_storage.hpp>

#include <iocoro/assert.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// Native socket address types (POSIX).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace iocoro::ip {
namespace detail {

namespace {

inline auto parse_port(std::string_view p) -> expected<std::uint16_t, std::error_code> {
  if (p.empty()) {
    return unexpected(error::invalid_argument);
  }
  unsigned value = 0;
  auto const* first = p.data();
  auto const* last = p.data() + p.size();
  auto r = std::from_chars(first, last, value);
  if (r.ec != std::errc{} || r.ptr != last || value > 65535u) {
    return unexpected(error::invalid_argument);
  }
  return static_cast<std::uint16_t>(value);
}

}  // namespace

inline endpoint_storage::endpoint_storage() noexcept { init_v4(address_v4::any(), 0); }

inline endpoint_storage::endpoint_storage(address_v4 addr, std::uint16_t port) noexcept {
  init_v4(addr, port);
}

inline endpoint_storage::endpoint_storage(address_v6 addr, std::uint16_t port) noexcept {
  init_v6(addr, port);
}

inline endpoint_storage::endpoint_storage(ip::address addr, std::uint16_t port) noexcept {
  if (addr.is_v4()) {
    init_v4(addr.to_v4(), port);
  } else {
    init_v6(addr.to_v6(), port);
  }
}

inline auto endpoint_storage::address() const noexcept -> ip::address {
  IOCORO_ASSERT(family() == AF_INET || family() == AF_INET6,
                "endpoint_storage::address(): invalid address family");
  if (family() == AF_INET) {
    auto const* sa = reinterpret_cast<sockaddr_in const*>(&storage_);
    address_v4::bytes_type b{};
    std::memcpy(b.data(), &sa->sin_addr.s_addr, 4);
    return ip::address{address_v4{b}};
  }
  if (family() == AF_INET6) {
    auto const* sa = reinterpret_cast<sockaddr_in6 const*>(&storage_);
    address_v6::bytes_type b{};
    std::memcpy(b.data(), sa->sin6_addr.s6_addr, 16);
    return ip::address{address_v6{b, sa->sin6_scope_id}};
  }
  IOCORO_UNREACHABLE();
}

inline auto endpoint_storage::port() const noexcept -> std::uint16_t {
  IOCORO_ASSERT(family() == AF_INET || family() == AF_INET6,
                "endpoint_storage::port(): invalid address family");
  if (family() == AF_INET) {
    auto const* sa = reinterpret_cast<sockaddr_in const*>(&storage_);
    return ntohs(sa->sin_port);
  }
  if (family() == AF_INET6) {
    auto const* sa = reinterpret_cast<sockaddr_in6 const*>(&storage_);
    return ntohs(sa->sin6_port);
  }
  IOCORO_UNREACHABLE();
}

inline auto endpoint_storage::data() const noexcept -> sockaddr const* {
  return reinterpret_cast<sockaddr const*>(&storage_);
}

inline auto endpoint_storage::data() noexcept -> sockaddr* {
  return reinterpret_cast<sockaddr*>(&storage_);
}

inline auto endpoint_storage::size() const noexcept -> socklen_t { return size_; }

inline auto endpoint_storage::family() const noexcept -> int {
  return static_cast<int>(storage_.ss_family);
}

inline auto endpoint_storage::to_native(sockaddr* addr, socklen_t len) const noexcept
  -> expected<socklen_t, std::error_code> {
  if (addr == nullptr || len == 0) {
    return unexpected(error::invalid_argument);
  }
  if (static_cast<std::size_t>(len) < static_cast<std::size_t>(size_)) {
    return unexpected(error::invalid_endpoint);
  }
  std::memcpy(addr, data(), static_cast<std::size_t>(size_));
  return size_;
}

inline auto endpoint_storage::to_string() const -> std::string {
  auto addr_str = address().to_string();
  if (family() == AF_INET6) {
    return "[" + addr_str + "]:" + std::to_string(port());
  }
  return addr_str + ":" + std::to_string(port());
}

inline auto endpoint_storage::from_string(std::string_view s)
  -> expected<endpoint_storage, std::error_code> {
  if (s.empty()) {
    return unexpected(error::invalid_argument);
  }

  // Bracketed IPv6: [addr]:port
  if (s.front() == '[') {
    auto const close = s.find(']');
    if (close == std::string_view::npos || close + 2 > s.size() || s[close + 1] != ':') {
      return unexpected(error::invalid_argument);
    }
    auto host = s.substr(1, close - 1);
    auto port_str = s.substr(close + 2);

    auto port = parse_port(port_str);
    if (!port) { return unexpected(port.error()); }

    // Force IPv6 parsing for bracketed form.
    auto a6 = address_v6::from_string(host);
    if (!a6) { return unexpected(a6.error()); }
    return endpoint_storage{*a6, *port};
  }

  // IPv4: host:port (reject raw IPv6 without brackets).
  auto const pos = s.rfind(':');
  if (pos == std::string_view::npos) {
    return unexpected(error::invalid_argument);
  }
  auto host = s.substr(0, pos);
  auto port_str = s.substr(pos + 1);

  // If host contains ':' here, it's an unbracketed IPv6; reject.
  if (host.find(':') != std::string_view::npos) {
    return unexpected(error::invalid_argument);
  }

  auto port = parse_port(port_str);
  if (!port) { return unexpected(port.error()); }

  auto a4 = address_v4::from_string(host);
  if (!a4) { return unexpected(a4.error()); }
  return endpoint_storage{*a4, *port};
}

inline auto endpoint_storage::from_native(sockaddr const* addr, socklen_t len)
  -> expected<endpoint_storage, std::error_code> {
  if (addr == nullptr || len <= 0) {
    return unexpected(error::invalid_argument);
  }
  if (len > sizeof(sockaddr_storage)) {
    return unexpected(error::invalid_endpoint);
  }
  if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
    return unexpected(error::unsupported_address_family);
  }

  // Enforce that the provided sockaddr is "complete" for its family.
  // We accept len >= sizeof(sockaddr_in[_6]) and normalize to the canonical size.
  auto const required_len =
    (addr->sa_family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
  if (static_cast<std::size_t>(len) < required_len) {
    return unexpected(error::invalid_endpoint);
  }

  endpoint_storage ep{};
  std::memset(&ep.storage_, 0, sizeof(ep.storage_));
  std::memcpy(&ep.storage_, addr, required_len);
  ep.size_ = static_cast<socklen_t>(required_len);
  return ep;
}

inline auto operator==(endpoint_storage const& a, endpoint_storage const& b) noexcept -> bool {
  if (a.size_ != b.size_) { return false; }
  return std::memcmp(&a.storage_, &b.storage_, a.size_) == 0;
}

inline auto operator<=>(endpoint_storage const& a, endpoint_storage const& b) noexcept
  -> std::strong_ordering {
  if (auto cmp = a.family() <=> b.family(); cmp != 0) { return cmp; }
  if (auto cmp = a.address() <=> b.address(); cmp != 0) { return cmp; }
  return a.port() <=> b.port();
}

inline void endpoint_storage::init_v4(address_v4 addr, std::uint16_t port) noexcept {
  auto sa = sockaddr_in{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);

  auto const b = addr.to_bytes();
  // b is in network byte order already; copy as-is.
  static_assert(sizeof(sa.sin_addr.s_addr) == 4);
  std::memcpy(&sa.sin_addr.s_addr, b.data(), 4);

  std::memcpy(&storage_, &sa, sizeof(sa));
  size_ = sizeof(sockaddr_in);
}

inline void endpoint_storage::init_v6(address_v6 addr, std::uint16_t port) noexcept {
  auto sa = sockaddr_in6{};
  sa.sin6_family = AF_INET6;
  sa.sin6_port = htons(port);

  auto const b = addr.to_bytes();
  static_assert(sizeof(sa.sin6_addr.s6_addr) == 16);
  std::memcpy(sa.sin6_addr.s6_addr, b.data(), 16);
  sa.sin6_scope_id = addr.scope_id();

  std::memcpy(&storage_, &sa, sizeof(sa));
  size_ = sizeof(sockaddr_in6);
}

}  // namespace detail
}  // namespace iocoro::ip
