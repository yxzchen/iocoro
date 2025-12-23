#pragma once

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>

// inet_pton / inet_ntop
#include <arpa/inet.h>
#include <netinet/in.h>

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

  /// Human-readable representation. (Stub-friendly; implementation may evolve.)
  auto to_string() const -> std::string {
    auto addr = in_addr{};
    static_assert(sizeof(addr.s_addr) == 4);
    auto const b = to_bytes();
    std::memcpy(&addr.s_addr, b.data(), 4);

    char buf[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) == nullptr) {
      // Best-effort: should never fail for AF_INET + 4 bytes.
      return "0.0.0.0";
    }
    return std::string(buf);
  }

  /// Parse a textual IPv4 address.
  ///
  /// Returns invalid_argument on parse failure.
  static auto from_string(std::string_view s) -> expected<address_v4, std::error_code> {
    auto addr = in_addr{};
    // inet_pton expects a null-terminated string.
    auto tmp = std::string(s);
    if (::inet_pton(AF_INET, tmp.c_str(), &addr) != 1) {
      return unexpected<std::error_code>(error::invalid_argument);
    }
    bytes_type b{};
    std::memcpy(b.data(), &addr.s_addr, 4);
    return address_v4{b};
  }

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
  constexpr auto is_loopback() const noexcept -> bool { return bytes_ == loopback().bytes_; }

  auto to_string() const -> std::string {
    auto addr = in6_addr{};
    auto const b = to_bytes();
    static_assert(sizeof(addr.s6_addr) == 16);
    std::memcpy(addr.s6_addr, b.data(), 16);

    char buf[INET6_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET6, &addr, buf, sizeof(buf)) == nullptr) {
      return "::";
    }

    std::string out(buf);
    if (scope_id_ != 0) {
      out.push_back('%');
      out += std::to_string(scope_id_);
    }
    return out;
  }

  /// Parse a textual IPv6 address.
  ///
  /// Supports an optional numeric scope_id suffix: "fe80::1%2".
  /// Returns invalid_argument on parse failure.
  static auto from_string(std::string_view s) -> expected<address_v6, std::error_code> {
    std::uint32_t scope = 0;
    auto ip_part = s;

    if (auto const pos = s.find('%'); pos != std::string_view::npos) {
      ip_part = s.substr(0, pos);
      auto scope_part = s.substr(pos + 1);
      if (scope_part.empty()) {
        return unexpected<std::error_code>(error::invalid_argument);
      }
      auto* first = scope_part.data();
      auto* last = scope_part.data() + scope_part.size();
      auto r = std::from_chars(first, last, scope);
      if (r.ec != std::errc{} || r.ptr != last) {
        return unexpected<std::error_code>(error::invalid_argument);
      }
    }

    auto addr = in6_addr{};
    auto tmp = std::string(ip_part);
    if (::inet_pton(AF_INET6, tmp.c_str(), &addr) != 1) {
      return unexpected<std::error_code>(error::invalid_argument);
    }

    bytes_type b{};
    std::memcpy(b.data(), addr.s6_addr, 16);
    return address_v6{b, scope};
  }

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

  constexpr auto is_v4() const noexcept -> bool {
    return std::holds_alternative<address_v4>(storage_);
  }
  constexpr auto is_v6() const noexcept -> bool {
    return std::holds_alternative<address_v6>(storage_);
  }

  constexpr auto to_v4() const -> address_v4 { return std::get<address_v4>(storage_); }
  constexpr auto to_v6() const -> address_v6 { return std::get<address_v6>(storage_); }

  auto to_string() const -> std::string {
    if (is_v4()) return std::get<address_v4>(storage_).to_string();
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
