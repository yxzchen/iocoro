#pragma once

#include <iocoro/ip/detail/address_v4.hpp>
#include <iocoro/ip/detail/address_v6.hpp>

#include <string>
#include <string_view>
#include <variant>

namespace iocoro::ip {

// Re-export detail types to ip namespace for public API
using address_v4 = ::iocoro::ip::detail::address_v4;
using address_v6 = ::iocoro::ip::detail::address_v6;

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
  static auto from_string(std::string const& s) -> expected<address, std::error_code> {
    if (s.find(':') != std::string::npos) {
      return address_v6::from_string(s).transform([](address_v6 a) { return address{a}; });
    }
    return address_v4::from_string(s).transform([](address_v4 a) { return address{a}; });
  }

 private:
  std::variant<address_v4, address_v6> storage_;
};

}  // namespace iocoro::ip
