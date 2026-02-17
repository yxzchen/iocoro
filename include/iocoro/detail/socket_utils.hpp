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

inline auto map_socket_errno(int err) noexcept -> std::error_code {
  switch (err) {
    case EPIPE: {
      return error::broken_pipe;
    }
    case ECONNRESET: {
      return error::connection_reset;
    }
    case ECONNREFUSED: {
      return error::connection_refused;
    }
    case ECONNABORTED: {
      return error::connection_aborted;
    }
    case ETIMEDOUT: {
      return error::connection_timed_out;
    }
    case EHOSTUNREACH: {
      return error::host_unreachable;
    }
    case ENETUNREACH: {
      return error::network_unreachable;
    }
    case EADDRINUSE: {
      return error::address_in_use;
    }
    case EADDRNOTAVAIL: {
      return error::address_not_available;
    }
    case EMSGSIZE: {
      return error::message_size;
    }
    default: {
      return std::error_code(err, std::generic_category());
    }
  }
}

inline constexpr auto send_no_signal_flags() noexcept -> int {
#if defined(MSG_NOSIGNAL)
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

inline auto is_accept_transient_error(int err) noexcept -> bool {
  switch (err) {
    case ENETDOWN:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
      return true;
    default:
      return false;
  }
}

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
    return unexpected(map_socket_errno(errno));
  }
  return Endpoint::from_native(reinterpret_cast<sockaddr*>(&ss), len);
}

inline auto get_socket_family(int fd) -> result<int> {
  if (fd < 0) {
    return unexpected(error::not_open);
  }

  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    return unexpected(map_socket_errno(errno));
  }
  return static_cast<int>(ss.ss_family);
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
    return unexpected(map_socket_errno(errno));
  }
  return Endpoint::from_native(reinterpret_cast<sockaddr*>(&ss), len);
}

}  // namespace iocoro::detail::socket
