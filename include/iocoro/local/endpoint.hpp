#pragma once

#include <iocoro/error.hpp>
#include <iocoro/result.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <sys/un.h>

namespace iocoro::local {

/// Local (AF_UNIX) endpoint.
///
/// Semantics:
/// - Wraps a native `sockaddr_un` + length.
/// - Supports pathname and (on Linux) abstract namespace endpoints.
///
/// Error handling:
/// - `from_native()` is allowed to fail and returns an error_code (not UB).
class endpoint {
 public:
  endpoint() noexcept = default;

  /// Create a pathname endpoint (e.g. "/tmp/app.sock").
  ///
  /// Returns invalid_argument if the path is empty or doesn't fit.
  static auto from_path(std::string_view path) noexcept -> result<endpoint> {
    if (path.empty()) {
      return unexpected(error::invalid_argument);
    }

    // Must fit including NUL terminator.
    if (path.size() + 1 > sizeof(sockaddr_un::sun_path)) {
      return unexpected(error::invalid_argument);
    }

    endpoint ep{};
    ep.addr_.sun_family = AF_UNIX;
    std::memset(ep.addr_.sun_path, 0, sizeof(ep.addr_.sun_path));
    std::memcpy(ep.addr_.sun_path, path.data(), path.size());
    ep.size_ =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);  // include NUL
    return ep;
  }

#if defined(__linux__)
  /// Create a Linux abstract-namespace endpoint.
  ///
  /// `name` is the bytes after the leading NUL.
  /// Returns invalid_argument if name is empty or doesn't fit.
  static auto from_abstract(std::string_view name) noexcept -> result<endpoint> {
    if (name.empty()) {
      return unexpected(error::invalid_argument);
    }

    // First byte is NUL, rest is name bytes without NUL terminator.
    if (name.size() > sizeof(sockaddr_un::sun_path) - 1) {
      return unexpected(error::invalid_argument);
    }

    endpoint ep{};
    ep.addr_.sun_family = AF_UNIX;
    std::memset(ep.addr_.sun_path, 0, sizeof(ep.addr_.sun_path));
    ep.addr_.sun_path[0] = '\0';
    std::memcpy(ep.addr_.sun_path + 1, name.data(), name.size());
    ep.size_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return ep;
  }
#endif

  /// Construct from native sockaddr.
  ///
  /// Returns:
  /// - invalid_argument if addr is null or len == 0
  /// - unsupported_address_family if family != AF_UNIX
  /// - invalid_endpoint if len is not a valid sockaddr_un length
  static auto from_native(sockaddr const* addr, socklen_t len) noexcept
    -> result<endpoint> {
    if (addr == nullptr || len == 0) {
      return unexpected(error::invalid_argument);
    }
    if (addr->sa_family != AF_UNIX) {
      return unexpected(error::unsupported_address_family);
    }

    // Must include at least one byte of sun_path (pathname NUL or abstract leading NUL).
    auto const min_len = offsetof(sockaddr_un, sun_path) + 1;
    if (static_cast<std::size_t>(len) < min_len) {
      return unexpected(error::invalid_endpoint);
    }
    if (static_cast<std::size_t>(len) > sizeof(sockaddr_un)) {
      return unexpected(error::invalid_endpoint);
    }

    endpoint ep{};
    std::memset(&ep.addr_, 0, sizeof(ep.addr_));
    std::memcpy(&ep.addr_, addr, static_cast<std::size_t>(len));
    ep.size_ = len;

#if defined(__linux__)
    // Reject empty abstract name (sun_path[0] == '\0' and no further bytes).
    if (ep.addr_.sun_path[0] == '\0' &&
        static_cast<std::size_t>(len) == offsetof(sockaddr_un, sun_path) + 1) {
      return unexpected(error::invalid_endpoint);
    }
#endif

    return ep;
  }

  /// Copy the native sockaddr representation into the user-provided buffer.
  ///
  /// Returns:
  /// - invalid_argument if `addr` is null or `len == 0`
  /// - invalid_endpoint if `len < size()`
  auto to_native(sockaddr* addr, socklen_t len) const noexcept
    -> result<socklen_t> {
    if (addr == nullptr || len == 0) {
      return unexpected(error::invalid_argument);
    }
    if (static_cast<std::size_t>(len) < static_cast<std::size_t>(size_)) {
      return unexpected(error::invalid_endpoint);
    }
    std::memcpy(addr, data(), static_cast<std::size_t>(size_));
    return size_;
  }

  auto data() const noexcept -> sockaddr const* {
    return reinterpret_cast<sockaddr const*>(&addr_);
  }
  auto size() const noexcept -> socklen_t { return size_; }
  auto family() const noexcept -> int { return AF_UNIX; }

 private:
  sockaddr_un addr_{};
  socklen_t size_{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};
};

}  // namespace iocoro::local
