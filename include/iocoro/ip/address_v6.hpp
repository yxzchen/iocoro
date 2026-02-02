#pragma once

#include <iocoro/error.hpp>
#include <iocoro/result.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace iocoro::ip {

/// IPv6 address value type.
class address_v6 {
 public:
  using bytes_type = std::array<std::uint8_t, 16>;

  constexpr address_v6() noexcept = default;
  explicit constexpr address_v6(bytes_type bytes, std::uint32_t scope_id = 0) noexcept
      : bytes_(bytes), scope_id_(scope_id) {}

  static constexpr auto any() noexcept -> address_v6 { return address_v6{}; }
  static constexpr auto loopback() noexcept -> address_v6 {
    auto b = bytes_type{};
    b[15] = 1;
    return address_v6{b};
  }

  constexpr auto to_bytes() const noexcept -> bytes_type { return bytes_; }
  constexpr auto scope_id() const noexcept -> std::uint32_t { return scope_id_; }

  constexpr auto is_unspecified() const noexcept -> bool { return bytes_ == bytes_type{}; }
  constexpr auto is_loopback() const noexcept -> bool {
    return bytes_ == loopback().bytes_ && scope_id_ == 0;
  }

  friend constexpr auto operator==(address_v6 const&, address_v6 const&) noexcept -> bool = default;
  friend constexpr auto operator<=>(address_v6 const&, address_v6 const&) noexcept = default;

  auto to_string() const -> std::string;

  /// Parse a textual IPv6 address.
  ///
  /// Supports an optional numeric scope_id suffix: "fe80::1%2".
  /// Returns invalid_argument on parse failure.
  static auto from_string(std::string const& s) -> result<address_v6>;

 private:
  bytes_type bytes_{};
  std::uint32_t scope_id_{0};
};

}  // namespace iocoro::ip

#include <iocoro/ip/impl/address_v6.ipp>
