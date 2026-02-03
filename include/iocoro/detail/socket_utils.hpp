#pragma once

#include <iocoro/error.hpp>
#include <iocoro/result.hpp>

#include <cerrno>
#include <system_error>
#include <utility>

// Native socket APIs for endpoint conversion.
#include <fcntl.h>
#include <sys/socket.h>

namespace iocoro::detail::socket {

inline auto retry_fcntl(int fd, int cmd, long arg) noexcept -> int {
  for (;;) {
    int const r = ::fcntl(fd, cmd, arg);
    if (r >= 0) {
      return r;
    }
    if (errno == EINTR) {
      continue;
    }
    return r;
  }
}

inline auto set_nonblocking(int fd) noexcept -> bool {
  int flags = retry_fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  if ((flags & O_NONBLOCK) != 0) {
    return true;
  }
  return retry_fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline auto set_cloexec(int fd) noexcept -> bool {
  int flags = retry_fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  if ((flags & FD_CLOEXEC) != 0) {
    return true;
  }
  return retry_fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

template <class Endpoint>
auto get_local_endpoint(int fd) -> result<Endpoint> {
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
auto get_remote_endpoint(int fd) -> result<Endpoint> {
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
