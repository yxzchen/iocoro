
#include <xz/io/ip.hpp>

#include <arpa/inet.h>
#include <fmt/format.h>

#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace xz::io::ip {

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
  return fmt::format("{}.{}.{}.{}", bytes_[0], bytes_[1], bytes_[2], bytes_[3]);
}

auto address_v6::from_string(std::string_view str) -> address_v6 {
  in6_addr addr{};
  std::string s(str);
  if (::inet_pton(AF_INET6, s.c_str(), &addr) != 1) {
    throw std::invalid_argument("invalid IPv6 address");
  }

  return address_v6{std::bit_cast<bytes_type>(addr)};
}

auto address_v6::loopback() noexcept -> address_v6 {
  bytes_type bytes{};
  bytes[15] = 1;
  return address_v6{bytes};
}

auto address_v6::to_string() const -> std::string {
  char buf[INET6_ADDRSTRLEN];
  auto addr = std::bit_cast<in6_addr>(bytes_);

  if (::inet_ntop(AF_INET6, &addr, buf, sizeof(buf)) == nullptr) {
    throw std::runtime_error("inet_ntop failed");
  }

  return buf;
}

auto tcp_endpoint::to_string() const -> std::string {
  if (is_v6()) {
    return fmt::format("[{}]:{}", get_address_v6().to_string(), port_);
  }
  return fmt::format("{}:{}", get_address_v4().to_string(), port_);
}

}  // namespace xz::io::ip
