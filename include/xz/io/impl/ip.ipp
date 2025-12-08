#pragma once

#include <xz/io/ip.hpp>

#include <arpa/inet.h>

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace xz::io::ip {

// address_v4 implementation

address_v4::address_v4(uint32_t addr) {
  bytes_[0] = (addr >> 24) & 0xFF;
  bytes_[1] = (addr >> 16) & 0xFF;
  bytes_[2] = (addr >> 8) & 0xFF;
  bytes_[3] = addr & 0xFF;
}

auto address_v4::from_string(std::string_view str) -> address_v4 {
  in_addr addr{};
  std::string s(str);
  if (::inet_pton(AF_INET, s.c_str(), &addr) != 1) {
    throw std::invalid_argument("invalid IPv4 address");
  }

  uint32_t n = ntohl(addr.s_addr);
  return address_v4{n};
}

auto address_v4::to_uint() const noexcept -> uint32_t {
  return (static_cast<uint32_t>(bytes_[0]) << 24) | (static_cast<uint32_t>(bytes_[1]) << 16) |
         (static_cast<uint32_t>(bytes_[2]) << 8) | static_cast<uint32_t>(bytes_[3]);
}

auto address_v4::to_string() const -> std::string {
  std::ostringstream oss;
  oss << static_cast<int>(bytes_[0]) << '.' << static_cast<int>(bytes_[1]) << '.'
      << static_cast<int>(bytes_[2]) << '.' << static_cast<int>(bytes_[3]);
  return oss.str();
}

// address_v6 implementation

auto address_v6::from_string(std::string_view str) -> address_v6 {
  in6_addr addr{};
  std::string s(str);
  if (::inet_pton(AF_INET6, s.c_str(), &addr) != 1) {
    throw std::invalid_argument("invalid IPv6 address");
  }

  bytes_type bytes;
  std::memcpy(bytes.data(), &addr, 16);
  return address_v6{bytes};
}

auto address_v6::loopback() noexcept -> address_v6 {
  bytes_type bytes{};
  bytes[15] = 1;
  return address_v6{bytes};
}

auto address_v6::to_string() const -> std::string {
  char buf[INET6_ADDRSTRLEN];
  in6_addr addr{};
  std::memcpy(&addr, bytes_.data(), 16);

  if (::inet_ntop(AF_INET6, &addr, buf, sizeof(buf)) == nullptr) {
    throw std::runtime_error("inet_ntop failed");
  }

  return buf;
}

// tcp_endpoint implementation

auto tcp_endpoint::to_string() const -> std::string {
  std::ostringstream oss;
  if (is_v6()) {
    oss << '[' << get_address_v6().to_string() << "]:" << port_;
  } else {
    oss << get_address_v4().to_string() << ':' << port_;
  }
  return oss.str();
}

}  // namespace xz::io::ip
