#pragma once

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>

#include <cerrno>
#include <system_error>

// Native socket APIs for endpoint conversion.
#include <sys/socket.h>

namespace iocoro::detail::socket {

template <class Endpoint>
auto get_local_endpoint(int fd) -> expected<Endpoint, std::error_code> {
  if (fd < 0) {
    return unexpected(error::not_open);
  }

  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }
  return Endpoint::from_native(reinterpret_cast<sockaddr*>(&ss), len);
}

template <class Endpoint>
auto get_remote_endpoint(int fd) -> expected<Endpoint, std::error_code> {
  if (fd < 0) {
    return unexpected(error::not_open);
  }

  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    if (errno == ENOTCONN) {
      return unexpected(error::not_connected);
    }
    return unexpected(std::error_code(errno, std::generic_category()));
  }
  return Endpoint::from_native(reinterpret_cast<sockaddr*>(&ss), len);
}

}  // namespace iocoro::detail::socket
