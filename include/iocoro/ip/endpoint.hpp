#pragma once

#include <iocoro/ip/address.hpp>

#include <cstdint>
#include <cstring>
#include <string>

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
  endpoint() noexcept { init_v4(address_v4::any(), 0); }

  endpoint(address_v4 addr, std::uint16_t port) noexcept { init_v4(addr, port); }

  endpoint(address_v6 addr, std::uint16_t port) noexcept { init_v6(addr, port); }

  endpoint(iocoro::ip::address addr, std::uint16_t port) noexcept {
    if (addr.is_v4()) {
      init_v4(addr.to_v4(), port);
    } else {
      init_v6(addr.to_v6(), port);
    }
  }

  auto address() const noexcept -> iocoro::ip::address {
    if (family() == AF_INET) {
      auto const* sa = reinterpret_cast<sockaddr_in const*>(&storage_);
      address_v4::bytes_type b{};
      std::memcpy(b.data(), &sa->sin_addr.s_addr, 4);
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

  auto to_string() const -> std::string {
    return address().to_string() + ":" + std::to_string(port());
  }

  /// Accessors for native interop.
  auto data() const noexcept -> sockaddr const*;
  auto size() const noexcept -> socklen_t;
  auto family() const noexcept -> int;

  friend auto operator==(endpoint const&, endpoint const&) noexcept -> bool = default;
  friend auto operator<=>(endpoint const&, endpoint const&) noexcept = default;

 private:
  void init_v4(address_v4 addr, std::uint16_t port) noexcept {
    auto sa = sockaddr_in{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    auto const b = addr.to_bytes();
    // b is in network byte order already; copy as-is.
    static_assert(sizeof(sa.sin_addr.s_addr) == 4);
    std::memcpy(&sa.sin_addr.s_addr, b.data(), 4);

    std::memcpy(&storage_, &sa, sizeof(sa));
    len_ = sizeof(sockaddr_in);
  }

  void init_v6(address_v6 addr, std::uint16_t port) noexcept {
    auto sa = sockaddr_in6{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);

    auto const b = addr.to_bytes();
    static_assert(sizeof(sa.sin6_addr.s6_addr) == 16);
    std::memcpy(sa.sin6_addr.s6_addr, b.data(), 16);
    sa.sin6_scope_id = addr.scope_id();

    std::memcpy(&storage_, &sa, sizeof(sa));
    len_ = sizeof(sockaddr_in6);
  }

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
