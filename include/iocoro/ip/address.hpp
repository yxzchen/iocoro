#pragma once

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>

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
  static auto from_string(std::string_view s) -> expected<address_v4, std::error_code>;

 private:
  bytes_type bytes_{};
};

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
  static auto from_string(std::string_view s) -> expected<address_v6, std::error_code>;

 private:
  bytes_type bytes_{};
  std::uint32_t scope_id_{0};
};

/// Generic IP address value type (v4 or v6).
class address {
 public:
  constexpr address() noexcept : storage_(address_v4{}) {}
  constexpr address(address_v4 v4) noexcept : storage_(v4) {}
  constexpr address(address_v6 v6) noexcept : storage_(v6) {}

  friend constexpr auto operator==(address const&, address const&) noexcept -> bool = default;
  friend constexpr auto operator<=>(address const&, address const&) noexcept = default;

  constexpr auto is_v4() const noexcept -> bool {
    return std::holds_alternative<address_v4>(storage_);
  }
  constexpr auto is_v6() const noexcept -> bool {
    return std::holds_alternative<address_v6>(storage_);
  }

  constexpr auto to_v4() const -> address_v4 { return std::get<address_v4>(storage_); }
  constexpr auto to_v6() const -> address_v6 { return std::get<address_v6>(storage_); }

  auto to_string() const -> std::string {
    if (is_v4()) {
      return std::get<address_v4>(storage_).to_string();
    }
    return std::get<address_v6>(storage_).to_string();
  }

  /// Parse a textual IP address (v4 or v6).
  ///
  /// Selection:
  /// - If the string contains ':', it is treated as IPv6.
  /// - Otherwise, IPv4.
  static auto from_string(std::string_view s) -> expected<address, std::error_code> {
    if (s.find(':') != std::string_view::npos) {
      return address_v6::from_string(s).transform([](address_v6 a) { return address{a}; });
    }
    return address_v4::from_string(s).transform([](address_v4 a) { return address{a}; });
  }

 private:
  std::variant<address_v4, address_v6> storage_;
};

}  // namespace iocoro::ip
