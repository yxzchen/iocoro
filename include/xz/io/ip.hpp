#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

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

  auto to_bytes() const noexcept -> bytes_type { return bytes_; }
  auto to_uint() const noexcept -> uint32_t;
  auto to_string() const -> std::string;

  auto operator==(address_v4 const& other) const noexcept -> bool { return bytes_ == other.bytes_; }
  auto operator!=(address_v4 const& other) const noexcept -> bool { return !(*this == other); }

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

  auto to_bytes() const noexcept -> bytes_type { return bytes_; }
  auto to_string() const -> std::string;

  auto operator==(address_v6 const& other) const noexcept -> bool { return bytes_ == other.bytes_; }
  auto operator!=(address_v6 const& other) const noexcept -> bool { return !(*this == other); }

 private:
  bytes_type bytes_{};
};

/// TCP endpoint (address + port)
class tcp_endpoint {
 public:
  tcp_endpoint() = default;
  tcp_endpoint(address_v4 addr, uint16_t port) : addr_v4_(addr), port_(port), is_v6_(false) {}
  tcp_endpoint(address_v6 addr, uint16_t port) : addr_v6_(addr), port_(port), is_v6_(true) {}

  auto get_address_v4() const -> ip::address_v4 { return addr_v4_; }
  auto get_address_v6() const -> ip::address_v6 { return addr_v6_; }
  auto port() const noexcept -> uint16_t { return port_; }
  auto is_v6() const noexcept -> bool { return is_v6_; }

  void port(uint16_t p) noexcept { port_ = p; }

  auto to_string() const -> std::string;

 private:
  ip::address_v4 addr_v4_;
  ip::address_v6 addr_v6_;
  uint16_t port_ = 0;
  bool is_v6_ = false;
};

}  // namespace xz::io::ip
