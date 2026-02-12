#include <iocoro/detail/socket/stream_socket_impl.hpp>

#include <cerrno>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace iocoro::detail::socket {

inline void stream_socket_impl::cancel() noexcept {
  read_op_.cancel();
  write_op_.cancel();
  connect_op_.cancel();
  base_.cancel();
}

inline void stream_socket_impl::cancel_read() noexcept {
  read_op_.cancel();
  base_.cancel_read();
}

inline void stream_socket_impl::cancel_write() noexcept {
  write_op_.cancel();
  base_.cancel_write();
}

inline void stream_socket_impl::cancel_connect() noexcept {
  connect_op_.cancel();
  // connect waits for writability.
  base_.cancel_write();
}

inline auto stream_socket_impl::close() noexcept -> result<void> {
  {
    std::scoped_lock lk{mtx_};
    read_op_.cancel();
    write_op_.cancel();
    connect_op_.cancel();
    state_ = conn_state::disconnected;
    shutdown_ = {};
    // NOTE: do not touch active flags here; their owner is the coroutine.
  }

  // Best-effort: push peer-observable shutdown before logical close.
  auto res = base_.acquire_resource();
  if (res) {
    auto const fd = res->native_handle();
    if (fd >= 0) {
      (void)::shutdown(fd, SHUT_RDWR);
    }
  }

  return base_.close();
}

inline auto stream_socket_impl::bind(sockaddr const* addr, socklen_t len) -> result<void> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    return fail(error::not_open);
  }
  if (res->closing()) {
    return fail(error::operation_aborted);
  }

  if (::bind(res->native_handle(), addr, len) != 0) {
    return fail(map_socket_errno(errno));
  }
  return ok();
}

inline auto stream_socket_impl::async_connect(sockaddr const* addr, socklen_t len)
  -> awaitable<result<void>> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    co_return fail(error::not_open);
  }
  if (res->closing()) {
    co_return fail(error::operation_aborted);
  }

  auto inflight = base_.make_operation_guard(res);
  auto const fd = res->native_handle();
  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (connect_op_.active) {
      co_return fail(error::busy);
    }
    if (state_ == conn_state::connecting) {
      co_return fail(error::busy);
    }
    if (state_ == conn_state::connected) {
      co_return fail(error::already_connected);
    }
    connect_op_.active = true;
    state_ = conn_state::connecting;
    my_epoch = connect_op_.epoch.load(std::memory_order_acquire);
  }

  auto connect_guard = detail::make_scope_exit([this] { connect_op_.finish(mtx_); });
  auto ec = std::error_code{};

  for (;;) {
    if (!connect_op_.is_epoch_current(my_epoch) || res->closing()) {
      std::scoped_lock lk{mtx_};
      state_ = conn_state::disconnected;
      co_return fail(error::operation_aborted);
    }

    if (::connect(fd, addr, len) == 0) {
      std::scoped_lock lk{mtx_};
      state_ = conn_state::connected;
      co_return ok();
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EINPROGRESS) {
      break;
    }
    ec = map_socket_errno(errno);
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(ec);
  }

  int so_error = 0;
  socklen_t optlen = sizeof(so_error);

  auto is_loopback = [](sockaddr const* sa, socklen_t salen) noexcept -> bool {
    if (!sa) {
      return false;
    }
    if (sa->sa_family == AF_INET) {
      if (salen < sizeof(sockaddr_in)) {
        return false;
      }
      auto const* in = reinterpret_cast<sockaddr_in const*>(sa);
      auto const addr_v4 = ntohl(in->sin_addr.s_addr);
      return (addr_v4 >> 24) == 127;
    }
    if (sa->sa_family == AF_INET6) {
      if (salen < sizeof(sockaddr_in6)) {
        return false;
      }
      auto const* in6 = reinterpret_cast<sockaddr_in6 const*>(sa);
      return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr) != 0;
    }
    return false;
  };

  if (is_loopback(addr, len)) {
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0) {
      ec = map_socket_errno(errno);
      std::scoped_lock lk{mtx_};
      state_ = conn_state::disconnected;
      co_return fail(ec);
    }
    if (so_error != 0 && so_error != EINPROGRESS) {
      ec = map_socket_errno(so_error);
      std::scoped_lock lk{mtx_};
      state_ = conn_state::disconnected;
      co_return fail(ec);
    }
    if (so_error == 0) {
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof(peer);
      if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
        std::scoped_lock lk{mtx_};
        if (!connect_op_.is_epoch_current(my_epoch) || res->closing()) {
          state_ = conn_state::disconnected;
          co_return fail(error::operation_aborted);
        }
        state_ = conn_state::connected;
        co_return ok();
      }
      if (errno != ENOTCONN) {
        ec = map_socket_errno(errno);
        std::scoped_lock lk{mtx_};
        state_ = conn_state::disconnected;
        co_return fail(ec);
      }
    }
  }

  auto wait_r = co_await base_.wait_write_ready(res);
  if (!wait_r) {
    if (wait_r.error() != error::eof && wait_r.error() != error::connection_reset) {
      std::scoped_lock lk{mtx_};
      state_ = conn_state::disconnected;
      co_return fail(wait_r.error());
    }
  }

  if (!connect_op_.is_epoch_current(my_epoch) || res->closing()) {
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(error::operation_aborted);
  }

  so_error = 0;
  optlen = sizeof(so_error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0) {
    ec = map_socket_errno(errno);
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(ec);
  }
  if (so_error != 0) {
    ec = map_socket_errno(so_error);
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(ec);
  }

  {
    std::scoped_lock lk{mtx_};
    if (!connect_op_.is_epoch_current(my_epoch) || res->closing()) {
      state_ = conn_state::disconnected;
      co_return fail(error::operation_aborted);
    }
    state_ = conn_state::connected;
  }
  co_return ok();
}

inline auto stream_socket_impl::async_read_some(std::span<std::byte> buffer)
  -> awaitable<result<std::size_t>> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    co_return unexpected(error::not_open);
  }

  auto inflight = base_.make_operation_guard(res);
  auto const fd = res->native_handle();

  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (state_ != conn_state::connected) {
      co_return unexpected(error::not_connected);
    }
    if (shutdown_.read) {
      co_return 0;
    }
    if (read_op_.active) {
      co_return unexpected(error::busy);
    }
    read_op_.active = true;
    my_epoch = read_op_.epoch.load(std::memory_order_acquire);
  }

  auto guard = detail::make_scope_exit([this] { read_op_.finish(mtx_); });

  if (buffer.empty()) {
    co_return 0;
  }

  for (;;) {
    if (!read_op_.is_epoch_current(my_epoch) || res->closing()) {
      co_return unexpected(error::operation_aborted);
    }

    auto n = ::read(fd, buffer.data(), buffer.size());
    if (n > 0) {
      co_return n;
    }
    if (n == 0) {
      co_return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      auto r = co_await base_.wait_read_ready(res);
      if (!r) {
        if (r.error() == error::eof) {
          co_return 0;
        }
        co_return unexpected(r.error());
      }
      if (!read_op_.is_epoch_current(my_epoch) || res->closing()) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }
    co_return unexpected(map_socket_errno(errno));
  }
}

inline auto stream_socket_impl::async_write_some(std::span<std::byte const> buffer)
  -> awaitable<result<std::size_t>> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    co_return unexpected(error::not_open);
  }

  auto inflight = base_.make_operation_guard(res);
  auto const fd = res->native_handle();

  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (state_ != conn_state::connected) {
      co_return unexpected(error::not_connected);
    }
    if (shutdown_.write) {
      co_return unexpected(error::broken_pipe);
    }
    if (write_op_.active) {
      co_return unexpected(error::busy);
    }
    write_op_.active = true;
    my_epoch = write_op_.epoch.load(std::memory_order_acquire);
  }

  auto guard = detail::make_scope_exit([this] { write_op_.finish(mtx_); });

  if (buffer.empty()) {
    co_return 0;
  }

  for (;;) {
    if (!write_op_.is_epoch_current(my_epoch) || res->closing()) {
      co_return unexpected(error::operation_aborted);
    }

    // On Linux, ::write() can raise SIGPIPE for closed peers.
    auto n = ::send(fd, buffer.data(), buffer.size(), MSG_NOSIGNAL);
    if (n >= 0) {
      co_return n;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      auto r = co_await base_.wait_write_ready(res);
      if (!r) {
        co_return unexpected(r.error());
      }
      if (!write_op_.is_epoch_current(my_epoch) || res->closing()) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }
    co_return unexpected(map_socket_errno(errno));
  }
}

inline auto stream_socket_impl::shutdown(shutdown_type what) -> result<void> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    return fail(error::not_open);
  }
  if (res->closing()) {
    return fail(error::operation_aborted);
  }

  int how = SHUT_RDWR;
  if (what == shutdown_type::receive) {
    how = SHUT_RD;
  } else if (what == shutdown_type::send) {
    how = SHUT_WR;
  } else {
    how = SHUT_RDWR;
  }

  if (::shutdown(res->native_handle(), how) != 0) {
    if (errno == ENOTCONN) {
      return fail(error::not_connected);
    }
    return fail(map_socket_errno(errno));
  }

  {
    std::scoped_lock lk{mtx_};
    if (what == shutdown_type::receive) {
      shutdown_.read = true;
    } else if (what == shutdown_type::send) {
      shutdown_.write = true;
    } else {
      shutdown_.read = true;
      shutdown_.write = true;
    }
  }
  return ok();
}

}  // namespace iocoro::detail::socket
