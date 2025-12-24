#pragma once

#include <iocoro/detail/ip/endpoint_base.hpp>

#include <utility>

namespace iocoro::ip::tcp {

/// TCP endpoint type.
///
/// This is a strong type wrapper around the shared `iocoro::detail::ip::endpoint_base`
/// implementation (asio/std::net style).
class endpoint {
 public:
  endpoint() noexcept = default;

  endpoint(address_v4 addr, std::uint16_t port) noexcept : base_(addr, port) {}
  endpoint(address_v6 addr, std::uint16_t port) noexcept : base_(addr, port) {}
  endpoint(iocoro::ip::address addr, std::uint16_t port) noexcept : base_(addr, port) {}

  explicit endpoint(iocoro::detail::ip::endpoint_base base) noexcept : base_(std::move(base)) {}

  static auto from_string(std::string_view s) -> expected<endpoint, std::error_code> {
    auto r = iocoro::detail::ip::endpoint_base::from_string(s);
    if (!r) return unexpected(r.error());
    return endpoint{*r};
  }

  static auto from_native(sockaddr const* addr, socklen_t len)
    -> expected<endpoint, std::error_code> {
    auto r = iocoro::detail::ip::endpoint_base::from_native(addr, len);
    if (!r) return unexpected(r.error());
    return endpoint{*r};
  }

  auto address() const noexcept -> iocoro::ip::address { return base_.address(); }
  auto port() const noexcept -> std::uint16_t { return base_.port(); }
  auto to_string() const -> std::string { return base_.to_string(); }

  auto family() const noexcept -> int { return base_.family(); }
  auto data() const noexcept -> sockaddr const* { return base_.data(); }
  auto size() const noexcept -> socklen_t { return base_.size(); }

  friend auto operator==(endpoint const& a, endpoint const& b) noexcept -> bool = default;
  friend auto operator<=>(endpoint const& a, endpoint const& b) noexcept
    -> std::strong_ordering = default;

 private:
  iocoro::detail::ip::endpoint_base base_{};
};

}  // namespace iocoro::ip::tcp
