#pragma once

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/ip/address.hpp>

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include <sys/socket.h>

namespace iocoro::ip {

auto parse_port(std::string_view p) -> expected<std::uint16_t, std::error_code>;

/// Shared endpoint implementation for IP protocols.
///
/// This is the single source of truth for socket-address storage, parsing, and
/// conversion. Protocol-specific endpoint types (e.g. tcp::endpoint) wrap this
/// type to provide strong typing without duplicating implementation.
class endpoint_base {
 public:
  endpoint_base() noexcept;

  endpoint_base(address_v4 addr, std::uint16_t port) noexcept;
  endpoint_base(address_v6 addr, std::uint16_t port) noexcept;
  endpoint_base(ip::address addr, std::uint16_t port) noexcept;

  auto address() const noexcept -> ip::address;
  auto port() const noexcept -> std::uint16_t;

  /// Accessors for native interop.
  auto data() const noexcept -> sockaddr const*;
  auto size() const noexcept -> socklen_t;
  auto family() const noexcept -> int;

  auto to_string() const -> std::string;

  /// Parse an endpoint from string.
  ///
  /// Supported forms:
  /// - "1.2.3.4:80"
  /// - "[::1]:80" (IPv6 must use brackets to avoid ambiguity)
  ///
  /// Returns invalid_argument on parse failure.
  static auto from_string(std::string_view s) -> expected<endpoint_base, std::error_code>;

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
    -> expected<endpoint_base, std::error_code>;

  friend auto operator==(endpoint_base const& a, endpoint_base const& b) noexcept -> bool;
  friend auto operator<=>(endpoint_base const& a, endpoint_base const& b) noexcept
    -> std::strong_ordering;

 private:
  void init_v4(address_v4 addr, std::uint16_t port) noexcept;
  void init_v6(address_v6 addr, std::uint16_t port) noexcept;

  sockaddr_storage storage_{};
  socklen_t size_{0};
};

}  // namespace iocoro::ip
