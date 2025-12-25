#pragma once

#include <iocoro/ip/endpoint_base.hpp>

#include <utility>

namespace iocoro::ip::udp {

/// UDP endpoint type.
///
/// This is a strong type wrapper around the shared `iocoro::ip::endpoint_base` implementation.
class endpoint {
 public:
  endpoint() noexcept = default;
  explicit endpoint(endpoint_base base) noexcept : base_(std::move(base)) {}

  endpoint(address_v4 addr, std::uint16_t port) noexcept : base_(addr, port) {}
  endpoint(address_v6 addr, std::uint16_t port) noexcept : base_(addr, port) {}
  endpoint(ip::address addr, std::uint16_t port) noexcept : base_(addr, port) {}

  auto address() const noexcept -> ip::address { return base_.address(); }
  auto port() const noexcept -> std::uint16_t { return base_.port(); }

  auto family() const noexcept -> int { return base_.family(); }
  auto data() const noexcept -> sockaddr const* { return base_.data(); }
  auto size() const noexcept -> socklen_t { return base_.size(); }

  auto to_string() const -> std::string { return base_.to_string(); }

  static auto from_string(std::string_view s) -> expected<endpoint, std::error_code> {
    auto r = endpoint_base::from_string(s);
    if (!r) return unexpected(r.error());
    return endpoint{*r};
  }

  static auto from_native(sockaddr const* addr, socklen_t len)
    -> expected<endpoint, std::error_code> {
    auto r = endpoint_base::from_native(addr, len);
    if (!r) return unexpected(r.error());
    return endpoint{*r};
  }

  friend auto operator==(endpoint const& a, endpoint const& b) noexcept -> bool = default;
  friend auto operator<=>(endpoint const& a, endpoint const& b) noexcept
    -> std::strong_ordering = default;

 private:
  endpoint_base base_{};
};

}  // namespace iocoro::ip::udp
