#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace xz::io::ip {

/// IPv4 address
class address_v4 {
 public:
  using bytes_type = std::array<uint8_t, 4>;

  address_v4() = default;
  explicit address_v4(bytes_type const& bytes) : bytes_(bytes) {}
  explicit address_v4(uint32_t addr);

  static auto from_string(std::string_view str) -> address_v4;
  static auto any() noexcept -> address_v4 { return address_v4{}; }
  static auto loopback() noexcept -> address_v4 { return address_v4{{127, 0, 0, 1}}; }

  [[nodiscard]] auto to_bytes() const noexcept -> bytes_type { return bytes_; }
  [[nodiscard]] auto to_uint() const noexcept -> uint32_t;
  [[nodiscard]] auto to_string() const -> std::string;

  [[nodiscard]] auto operator<=>(address_v4 const&) const noexcept = default;

 private:
  bytes_type bytes_{};
};

/// IPv6 address
class address_v6 {
 public:
  using bytes_type = std::array<uint8_t, 16>;

  address_v6() = default;
  explicit address_v6(bytes_type const& bytes) : bytes_(bytes) {}

  static auto from_string(std::string_view str) -> address_v6;
  static auto any() noexcept -> address_v6 { return address_v6{}; }
  static auto loopback() noexcept -> address_v6;

  [[nodiscard]] auto to_bytes() const noexcept -> bytes_type { return bytes_; }
  [[nodiscard]] auto to_string() const -> std::string;

  [[nodiscard]] auto operator<=>(address_v6 const&) const noexcept = default;

 private:
  bytes_type bytes_{};
};

/// TCP endpoint (address + port)
class tcp_endpoint {
 public:
  tcp_endpoint() = default;
  tcp_endpoint(address_v4 addr, uint16_t port) : addr_(addr), port_(port) {}
  tcp_endpoint(address_v6 addr, uint16_t port) : addr_(addr), port_(port) {}

  [[nodiscard]] auto get_address_v4() const noexcept -> address_v4 const& { return std::get<address_v4>(addr_); }

  [[nodiscard]] auto get_address_v6() const noexcept -> address_v6 const& { return std::get<address_v6>(addr_); }

  [[nodiscard]] auto address() const noexcept -> std::variant<address_v4, address_v6> const& { return addr_; }

  [[nodiscard]] auto port() const noexcept -> uint16_t { return port_; }
  [[nodiscard]] auto is_v4() const noexcept -> bool { return std::holds_alternative<address_v4>(addr_); }
  [[nodiscard]] auto is_v6() const noexcept -> bool { return std::holds_alternative<address_v6>(addr_); }

  void set_port(uint16_t p) noexcept { port_ = p; }

  [[nodiscard]] auto to_string() const -> std::string;

 private:
  std::variant<address_v4, address_v6> addr_;
  uint16_t port_ = 0;
};

}  // namespace xz::io::ip
