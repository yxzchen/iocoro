#pragma once

#include <iocoro/error.hpp>
#include <iocoro/ip/address.hpp>
#include <iocoro/result.hpp>

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include <sys/socket.h>

namespace iocoro::ip::detail {

/// Protocol-agnostic storage for an IP endpoint (sockaddr_storage + helpers).
///
/// Responsibilities:
/// - Own sockaddr storage and length.
/// - Parse/format endpoints (string <-> native sockaddr).
/// - Provide accessors for address/port/family.
///
/// Non-responsibilities:
/// - MUST NOT depend on any Protocol tag or Protocol-specific typing.
class endpoint_storage {
 public:
  endpoint_storage() noexcept;

  endpoint_storage(address_v4 addr, std::uint16_t port) noexcept;
  endpoint_storage(address_v6 addr, std::uint16_t port) noexcept;
  endpoint_storage(ip::address addr, std::uint16_t port) noexcept;

  auto address() const noexcept -> ip::address;
  auto port() const noexcept -> std::uint16_t;

  auto data() const noexcept -> sockaddr const*;
  auto data() noexcept -> sockaddr*;

  auto size() const noexcept -> socklen_t;
  auto family() const noexcept -> int;

  auto to_string() const -> std::string;

  static auto from_string(std::string const& s) -> result<endpoint_storage>;
  static auto from_native(sockaddr const* addr, socklen_t len) -> result<endpoint_storage>;

  /// Copy the native sockaddr representation into the user-provided buffer.
  ///
  /// This is the dual of `from_native()`:
  /// - `addr` points to a writable buffer of length `len`.
  /// - On success, writes `size()` bytes and returns the number of bytes written.
  ///
  /// Returns:
  /// - invalid_argument if `addr` is null or `len == 0`
  /// - invalid_endpoint if `len < size()`
  auto to_native(sockaddr* addr, socklen_t len) const noexcept -> result<socklen_t>;

  /// Lexicographical ordering for endpoints.
  ///
  /// Order is: family, then address, then port.
  /// This is a semantic ordering (not a raw byte memcmp) and is intended to be stable.
  friend auto operator==(endpoint_storage const& a, endpoint_storage const& b) noexcept -> bool;
  friend auto operator<=>(endpoint_storage const& a, endpoint_storage const& b) noexcept
    -> std::strong_ordering;

 private:
  void init_v4(address_v4 addr, std::uint16_t port) noexcept;
  void init_v6(address_v6 addr, std::uint16_t port) noexcept;

  sockaddr_storage storage_{};
  socklen_t size_{0};
};

}  // namespace iocoro::ip::detail

#include <iocoro/ip/impl/endpoint_storage.ipp>
