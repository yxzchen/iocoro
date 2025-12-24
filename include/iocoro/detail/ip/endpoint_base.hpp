#pragma once

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/ip/address.hpp>

#include <charconv>
#include <compare>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// Native socket address types (POSIX).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace iocoro::detail::ip {

inline auto parse_port(std::string_view p) -> expected<std::uint16_t, std::error_code> {
  if (p.empty()) {
    return unexpected(error::invalid_argument);
  }
  unsigned value = 0;
  auto* first = p.data();
  auto* last = p.data() + p.size();
  auto r = std::from_chars(first, last, value);
  if (r.ec != std::errc{} || r.ptr != last || value > 65535u) {
    return unexpected(error::invalid_argument);
  }
  return static_cast<std::uint16_t>(value);
}

/// Shared endpoint implementation for IP protocols.
///
/// This is the single source of truth for socket-address storage, parsing, and
/// conversion. Protocol-specific endpoint types (e.g. tcp::endpoint) wrap this
/// type to provide strong typing without duplicating implementation.
class endpoint_base {
 public:
  endpoint_base() noexcept { init_v4(iocoro::ip::address_v4::any(), 0); }

  endpoint_base(iocoro::ip::address_v4 addr, std::uint16_t port) noexcept { init_v4(addr, port); }
  endpoint_base(iocoro::ip::address_v6 addr, std::uint16_t port) noexcept { init_v6(addr, port); }

  endpoint_base(iocoro::ip::address addr, std::uint16_t port) noexcept {
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
  static auto from_string(std::string_view s) -> expected<endpoint_base, std::error_code> {
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
      if (!port) {
        return unexpected(port.error());
      }

      // Force IPv6 parsing for bracketed form.
      auto a6 = iocoro::ip::address_v6::from_string(host);
      if (!a6) {
        return unexpected(a6.error());
      }
      return endpoint_base{*a6, *port};
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
    if (!port) {
      return unexpected(port.error());
    }

    auto a4 = iocoro::ip::address_v4::from_string(host);
    if (!a4) {
      return unexpected(a4.error());
    }
    return endpoint_base{*a4, *port};
  }

  /// Construct an endpoint from a native sockaddr.
  ///
  /// Preconditions:
  /// - `addr` points to a valid socket address of length `len`.
  /// - `len` must not exceed sizeof(sockaddr_storage).
  ///
  /// Returns:
  /// - endpoint_base on success
  /// - invalid_endpoint / unsupported_address_family / invalid_argument on failure
  static auto from_native(sockaddr const* addr, socklen_t len)
    -> expected<endpoint_base, std::error_code> {
    if (addr == nullptr || len <= 0) {
      return unexpected(error::invalid_argument);
    }
    if (len > sizeof(sockaddr_storage)) {
      return unexpected(error::invalid_endpoint);
    }
    if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
      return unexpected(error::unsupported_address_family);
    }

    endpoint_base ep{};
    std::memset(&ep.storage_, 0, sizeof(ep.storage_));
    std::memcpy(&ep.storage_, addr, static_cast<std::size_t>(len));
    ep.size_ = len;
    return ep;
  }

  auto address() const noexcept -> iocoro::ip::address {
    if (family() == AF_INET) {
      auto const* sa = reinterpret_cast<sockaddr_in const*>(&storage_);
      iocoro::ip::address_v4::bytes_type b{};
      std::memcpy(b.data(), &sa->sin_addr.s_addr, 4);
      return iocoro::ip::address{iocoro::ip::address_v4{b}};
    }
    if (family() == AF_INET6) {
      auto const* sa = reinterpret_cast<sockaddr_in6 const*>(&storage_);
      iocoro::ip::address_v6::bytes_type b{};
      std::memcpy(b.data(), sa->sin6_addr.s6_addr, 16);
      return iocoro::ip::address{iocoro::ip::address_v6{b, sa->sin6_scope_id}};
    }
    return iocoro::ip::address{iocoro::ip::address_v4::any()};
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
  auto data() const noexcept -> sockaddr const* {
    return reinterpret_cast<sockaddr const*>(&storage_);
  }
  auto size() const noexcept -> socklen_t { return size_; }
  auto family() const noexcept -> int { return static_cast<int>(storage_.ss_family); }

  friend auto operator==(endpoint_base const& a, endpoint_base const& b) noexcept -> bool {
    if (a.size_ != b.size_) return false;
    return std::memcmp(&a.storage_, &b.storage_, a.size_) == 0;
  }

  friend auto operator<=>(endpoint_base const& a, endpoint_base const& b) noexcept
    -> std::strong_ordering {
    if (auto cmp = a.family() <=> b.family(); cmp != 0) return cmp;
    if (auto cmp = a.address() <=> b.address(); cmp != 0) return cmp;
    return a.port() <=> b.port();
  }

 private:
  void init_v4(iocoro::ip::address_v4 addr, std::uint16_t port) noexcept {
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

  void init_v6(iocoro::ip::address_v6 addr, std::uint16_t port) noexcept {
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

  sockaddr_storage storage_{};
  socklen_t size_{0};
};

}  // namespace iocoro::detail::ip
