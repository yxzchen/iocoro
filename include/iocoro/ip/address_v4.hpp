#pragma once

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace iocoro::ip {

/// IPv4 address value type.
class address_v4 {
 public:
  using bytes_type = std::array<std::uint8_t, 4>;

  constexpr address_v4() noexcept = default;
  explicit constexpr address_v4(bytes_type bytes) noexcept : bytes_(bytes) {}

  static constexpr auto any() noexcept -> address_v4 { return address_v4{}; }
  static constexpr auto loopback() noexcept -> address_v4 { return address_v4{{127, 0, 0, 1}}; }

  constexpr auto to_bytes() const noexcept -> bytes_type { return bytes_; }

  constexpr auto is_unspecified() const noexcept -> bool { return bytes_ == bytes_type{}; }
  constexpr auto is_loopback() const noexcept -> bool { return bytes_ == loopback().bytes_; }

  friend constexpr auto operator==(address_v4 const&, address_v4 const&) noexcept -> bool = default;
  friend constexpr auto operator<=>(address_v4 const&, address_v4 const&) noexcept = default;

  /// Human-readable representation. (Stub-friendly; implementation may evolve.)
  auto to_string() const -> std::string;

  /// Parse a textual IPv4 address.
  ///
  /// Returns invalid_argument on parse failure.
  static auto from_string(std::string const& s) -> expected<address_v4, std::error_code>;

 private:
  bytes_type bytes_{};
};

}  // namespace iocoro::ip

#include <iocoro/ip/impl/address_v4.ipp>
