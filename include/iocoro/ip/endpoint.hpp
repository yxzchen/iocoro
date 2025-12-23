#pragma once

#include <iocoro/ip/address.hpp>

#include <cstdint>
#include <cstring>

// Native socket address types (POSIX).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace iocoro::ip {

/// Endpoint value type for IP sockets.
///
/// Design choice (per project decision):
/// - endpoint owns its OS-interoperable representation: sockaddr_storage + socklen_t.
/// - This keeps protocol-specific conversion out of impl layers.
class endpoint {
 public:
  endpoint() noexcept {
    // Default to an IPv4 "unspecified" endpoint with port 0.
    auto sa = sockaddr_in{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(0);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    storage_ = {};
    std::memcpy(&storage_, &sa, sizeof(sa));
    len_ = sizeof(sockaddr_in);
  }

  endpoint(address_v4 addr, std::uint16_t port) noexcept {
    auto sa = sockaddr_in{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    auto const b = addr.to_bytes();
    std::uint32_t v = (static_cast<std::uint32_t>(b[0]) << 24) |
                      (static_cast<std::uint32_t>(b[1]) << 16) |
                      (static_cast<std::uint32_t>(b[2]) << 8) | (static_cast<std::uint32_t>(b[3]));
    sa.sin_addr.s_addr = htonl(v);
    storage_ = {};
    std::memcpy(&storage_, &sa, sizeof(sa));
    len_ = sizeof(sockaddr_in);
  }

  endpoint(address_v6 addr, std::uint16_t port) noexcept {
    auto sa = sockaddr_in6{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    auto const b = addr.to_bytes();
    static_assert(sizeof(sa.sin6_addr.s6_addr) == 16);
    std::memcpy(sa.sin6_addr.s6_addr, b.data(), 16);
    sa.sin6_scope_id = addr.scope_id();
    storage_ = {};
    std::memcpy(&storage_, &sa, sizeof(sa));
    len_ = sizeof(sockaddr_in6);
  }

  endpoint(iocoro::ip::address addr, std::uint16_t port) noexcept {
    if (addr.is_v4()) {
      *this = endpoint(addr.to_v4(), port);
    } else {
      *this = endpoint(addr.to_v6(), port);
    }
  }

  auto address() const noexcept -> iocoro::ip::address {
    // Stub: decode only minimal cases.
    if (family() == AF_INET) {
      auto const* sa = reinterpret_cast<sockaddr_in const*>(&storage_);
      auto const raw = ntohl(sa->sin_addr.s_addr);
      address_v4::bytes_type b{
        static_cast<std::uint8_t>((raw >> 24) & 0xFF),
        static_cast<std::uint8_t>((raw >> 16) & 0xFF),
        static_cast<std::uint8_t>((raw >> 8) & 0xFF),
        static_cast<std::uint8_t>(raw & 0xFF),
      };
      return iocoro::ip::address{address_v4{b}};
    }
    if (family() == AF_INET6) {
      auto const* sa = reinterpret_cast<sockaddr_in6 const*>(&storage_);
      address_v6::bytes_type b{};
      std::memcpy(b.data(), sa->sin6_addr.s6_addr, 16);
      return iocoro::ip::address{address_v6{b, sa->sin6_scope_id}};
    }
    return iocoro::ip::address{address_v4::any()};
  }

  auto port() const noexcept -> std::uint16_t {
    if (family() == AF_INET) {
      auto const* sa = reinterpret_cast<sockaddr_in const*>(&storage_);
      return ntohs(sa->sin_port);
    }
    if (family() == AF_INET6) {
      auto const* sa = reinterpret_cast<sockaddr_in6 const*>(&storage_);
      return ntohs(sa->sin6_port);
    }
    return 0;
  }

  /// Accessors for native interop.
  auto data() const noexcept -> sockaddr const*;
  auto size() const noexcept -> socklen_t;
  auto family() const noexcept -> int;

 private:
  sockaddr_storage storage_{};
  socklen_t len_{0};
};

inline auto endpoint::data() const noexcept -> sockaddr const* {
  return reinterpret_cast<sockaddr const*>(&storage_);
}

inline auto endpoint::size() const noexcept -> socklen_t { return len_; }

inline auto endpoint::family() const noexcept -> int {
  return reinterpret_cast<sockaddr const*>(&storage_)->sa_family;
}

}  // namespace iocoro::ip
