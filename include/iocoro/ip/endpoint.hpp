#pragma once

#include <iocoro/error.hpp>
#include <iocoro/ip/address.hpp>
#include <iocoro/expected.hpp>

#include <charconv>
#include <compare>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>

// Native socket address types (POSIX).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace iocoro::ip {

namespace detail {

inline auto parse_port(std::string_view p) -> expected<std::uint16_t, std::error_code> {
  if (p.empty()) {
    return unexpected<std::error_code>(error::invalid_argument);
  }
  unsigned value = 0;
  auto* first = p.data();
  auto* last = p.data() + p.size();
  auto r = std::from_chars(first, last, value);
  if (r.ec != std::errc{} || r.ptr != last || value > 65535u) {
    return unexpected<std::error_code>(error::invalid_argument);
  }
  return static_cast<std::uint16_t>(value);
}

}  // namespace detail

/// Endpoint value type for IP sockets.
///
/// Design choice (per project decision):
/// - endpoint owns its OS-interoperable representation: sockaddr_storage + socklen_t.
/// - This keeps protocol-specific conversion out of impl layers.
class endpoint {
 public:
  endpoint() noexcept { init_v4(address_v4::any(), 0); }

  endpoint(address_v4 addr, std::uint16_t port) noexcept { init_v4(addr, port); }

  endpoint(address_v6 addr, std::uint16_t port) noexcept { init_v6(addr, port); }

  endpoint(iocoro::ip::address addr, std::uint16_t port) noexcept {
    if (addr.is_v4()) {
      init_v4(addr.to_v4(), port);
    } else {
      init_v6(addr.to_v6(), port);
    }
  }

  /// Parse an endpoint from string.
  ///
  /// Supported forms:
  /// - "1.2.3.4:80"
  /// - "[::1]:80" (IPv6 must use brackets to avoid ambiguity)
  ///
  /// Returns invalid_argument on parse failure.
  static auto from_string(std::string_view s) -> expected<endpoint, std::error_code> {
    if (s.empty()) {
      return unexpected<std::error_code>(error::invalid_argument);
    }

    // Bracketed IPv6: [addr]:port
    if (s.front() == '[') {
      auto const close = s.find(']');
      if (close == std::string_view::npos || close + 2 > s.size() || s[close + 1] != ':') {
        return unexpected<std::error_code>(error::invalid_argument);
      }
      auto host = s.substr(1, close - 1);
      auto port_str = s.substr(close + 2);

      auto port = detail::parse_port(port_str);
      if (!port) {
        return unexpected<std::error_code>(port.error());
      }

      // Force IPv6 parsing for bracketed form.
      auto a6 = address_v6::from_string(host);
      if (!a6) {
        return unexpected<std::error_code>(a6.error());
      }
      return endpoint{*a6, *port};
    }

    // IPv4: host:port (reject raw IPv6 without brackets).
    auto const pos = s.rfind(':');
    if (pos == std::string_view::npos) {
      return unexpected<std::error_code>(error::invalid_argument);
    }
    auto host = s.substr(0, pos);
    auto port_str = s.substr(pos + 1);

    // If host contains ':' here, it's an unbracketed IPv6; reject.
    if (host.find(':') != std::string_view::npos) {
      return unexpected<std::error_code>(error::invalid_argument);
    }

    auto port = detail::parse_port(port_str);
    if (!port) {
      return unexpected<std::error_code>(port.error());
    }

    auto a4 = address_v4::from_string(host);
    if (!a4) {
      return unexpected<std::error_code>(a4.error());
    }
    return endpoint{*a4, *port};
  }

  auto address() const noexcept -> iocoro::ip::address {
    if (family() == AF_INET) {
      auto const* sa = reinterpret_cast<sockaddr_in const*>(&storage_);
      address_v4::bytes_type b{};
      std::memcpy(b.data(), &sa->sin_addr.s_addr, 4);
      return iocoro::ip::address{address_v4{b}};
    }
    if (family() == AF_INET6) {
      auto const* sa = reinterpret_cast<sockaddr_in6 const*>(&storage_);
      address_v6::bytes_type b{};
      std::memcpy(b.data(), sa->sin6_addr.s6_addr, 16);
      return iocoro::ip::address{address_v6{b, sa->sin6_scope_id}};
    }
    return iocoro::ip::address{address_v4::any()};
  }

  auto port() const noexcept -> std::uint16_t {
    if (family() == AF_INET) {
      auto const* sa = reinterpret_cast<sockaddr_in const*>(&storage_);
      return ntohs(sa->sin_port);
    }
    if (family() == AF_INET6) {
      auto const* sa = reinterpret_cast<sockaddr_in6 const*>(&storage_);
      return ntohs(sa->sin6_port);
    }
    return 0;
  }

  auto to_string() const -> std::string {
    auto addr_str = address().to_string();
    if (family() == AF_INET6) {
      return "[" + addr_str + "]:" + std::to_string(port());
    }
    return addr_str + ":" + std::to_string(port());
  }

  /// Accessors for native interop.
  auto data() const noexcept -> sockaddr const*;
  auto size() const noexcept -> socklen_t;
  auto family() const noexcept -> int;

  friend auto operator==(endpoint const& a, endpoint const& b) noexcept -> bool {
    if (a.family() != b.family()) return false;
    return a.port() == b.port() && a.address() == b.address();
  }

  friend auto operator<=>(endpoint const& a, endpoint const& b) noexcept -> std::strong_ordering {
    return std::tuple{a.family(), a.address(), a.port()} <=> std::tuple{b.family(), b.address(), b.port()};
  }

 private:
  void init_v4(address_v4 addr, std::uint16_t port) noexcept {
    auto sa = sockaddr_in{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    auto const b = addr.to_bytes();
    // b is in network byte order already; copy as-is.
    static_assert(sizeof(sa.sin_addr.s_addr) == 4);
    std::memcpy(&sa.sin_addr.s_addr, b.data(), 4);

    std::memcpy(&storage_, &sa, sizeof(sa));
    len_ = sizeof(sockaddr_in);
  }

  void init_v6(address_v6 addr, std::uint16_t port) noexcept {
    auto sa = sockaddr_in6{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);

    auto const b = addr.to_bytes();
    static_assert(sizeof(sa.sin6_addr.s6_addr) == 16);
    std::memcpy(sa.sin6_addr.s6_addr, b.data(), 16);
    sa.sin6_scope_id = addr.scope_id();

    std::memcpy(&storage_, &sa, sizeof(sa));
    len_ = sizeof(sockaddr_in6);
  }

  sockaddr_storage storage_{};
  socklen_t len_{0};
};

inline auto endpoint::data() const noexcept -> sockaddr const* {
  return reinterpret_cast<sockaddr const*>(&storage_);
}

inline auto endpoint::size() const noexcept -> socklen_t { return len_; }

inline auto endpoint::family() const noexcept -> int {
  return static_cast<int>(storage_.ss_family);
}

}  // namespace iocoro::ip
