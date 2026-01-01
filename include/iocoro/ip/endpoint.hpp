#pragma once

// IP-domain endpoint.
//
// This type is IP-specific (parsing, formatting, address/port semantics).
// It is NOT a generic endpoint for non-IP domains (e.g. AF_UNIX).

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/ip/address.hpp>
#include <iocoro/ip/detail/endpoint_storage.hpp>

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include <sys/socket.h>

namespace iocoro::ip {

/// Strongly-typed IP endpoint for a given Protocol.
///
/// Layering / responsibilities:
/// - `iocoro::detail::basic_socket_handle<Impl>` (elsewhere) is a protocol-agnostic handle wrapper
///   used by socket-like facades.
/// - `iocoro::ip::endpoint<Protocol>` is the protocol-typed IP endpoint facade.
/// - The underlying storage and parsing logic lives in `iocoro::ip::detail::endpoint_storage`,
///   which MUST NOT depend on Protocol.
template <class Protocol>
class endpoint {
 public:
  using protocol_type = Protocol;

  endpoint() noexcept = default;

  endpoint(address_v4 addr, std::uint16_t port) noexcept : storage_(addr, port) {}
  endpoint(address_v6 addr, std::uint16_t port) noexcept : storage_(addr, port) {}
  endpoint(ip::address addr, std::uint16_t port) noexcept : storage_(addr, port) {}

  auto address() const noexcept -> ip::address { return storage_.address(); }
  auto port() const noexcept -> std::uint16_t { return storage_.port(); }

  /// Accessors for native interop.
  auto data() const noexcept -> sockaddr const* { return storage_.data(); }
  auto size() const noexcept -> socklen_t { return storage_.size(); }
  auto family() const noexcept -> int { return storage_.family(); }

  auto to_string() const -> std::string { return storage_.to_string(); }

  /// Parse an endpoint from string.
  ///
  /// Supported forms:
  /// - "1.2.3.4:80"
  /// - "[::1]:80" (IPv6 must use brackets to avoid ambiguity)
  ///
  /// Returns invalid_argument on parse failure.
  static auto from_string(std::string_view s) -> expected<endpoint, std::error_code> {
    auto r = detail::endpoint_storage::from_string(s);
    if (!r) {
      return unexpected(r.error());
    }
    return endpoint{std::move(*r)};
  }

  /// Construct an endpoint from a native sockaddr.
  ///
  /// Returns:
  /// - endpoint on success
  /// - invalid_endpoint / unsupported_address_family / invalid_argument on failure
  static auto from_native(sockaddr const* addr, socklen_t len)
    -> expected<endpoint, std::error_code> {
    auto r = detail::endpoint_storage::from_native(addr, len);
    if (!r) {
      return unexpected(r.error());
    }
    return endpoint{std::move(*r)};
  }

  /// Copy the native sockaddr representation into the user-provided buffer.
  /// See `detail::endpoint_storage::to_native()` for contract.
  auto to_native(sockaddr* addr, socklen_t len) const noexcept
    -> expected<socklen_t, std::error_code> {
    return storage_.to_native(addr, len);
  }

  friend auto operator==(endpoint const& a, endpoint const& b) noexcept -> bool {
    return a.storage_ == b.storage_;
  }
  friend auto operator<=>(endpoint const& a, endpoint const& b) noexcept -> std::strong_ordering {
    return a.storage_ <=> b.storage_;
  }

 private:
  explicit endpoint(detail::endpoint_storage st) noexcept : storage_(std::move(st)) {}

  detail::endpoint_storage storage_{};
};

}  // namespace iocoro::ip
