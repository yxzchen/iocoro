#include <iocoro/detail/socket/stream_socket_impl.hpp>

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

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
  return base_.close();
}

inline auto stream_socket_impl::bind(sockaddr const* addr, socklen_t len) -> result<void> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return fail(error::not_open);
  }
  if (::bind(fd, addr, len) != 0) {
    return fail(std::error_code(errno, std::generic_category()));
  }
  return ok();
}

inline auto stream_socket_impl::async_connect(sockaddr const* addr, socklen_t len)
  -> awaitable<result<void>> {
  if (!is_open()) {
    co_return fail(error::not_open);
  }

  auto const fd = base_.native_handle();
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

  // Ensure the "connect owner" flag is always released by the owning coroutine.
  auto connect_guard = detail::make_scope_exit([this] { connect_op_.finish(mtx_); });

  // We intentionally keep syscall logic outside the mutex.
  auto ec = std::error_code{};

  // Attempt immediate connect.
  for (;;) {
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
    ec = std::error_code(errno, std::generic_category());
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(ec);
  }

  // Wait for writability, then check SO_ERROR.
  auto wait_r = co_await base_.wait_write_ready();
  if (!wait_r) {
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(wait_r.error());
  }

  // If cancel()/close() happened while we were waiting, treat as aborted.
  if (!connect_op_.is_epoch_current(my_epoch)) {
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(error::operation_aborted);
  }

  int so_error = 0;
  socklen_t optlen = sizeof(so_error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0) {
    ec = std::error_code(errno, std::generic_category());
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(ec);
  }
  if (so_error != 0) {
    ec = std::error_code(so_error, std::generic_category());
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return fail(ec);
  }

  {
    std::scoped_lock lk{mtx_};
    if (!connect_op_.is_epoch_current(my_epoch)) {
      state_ = conn_state::disconnected;
      co_return fail(error::operation_aborted);
    }
    state_ = conn_state::connected;
  }
  co_return ok();
}

inline auto stream_socket_impl::async_read_some(std::span<std::byte> buffer)
  -> awaitable<result<std::size_t>> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    co_return unexpected(error::not_open);
  }

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
    auto n = ::read(fd, buffer.data(), buffer.size());
    if (n > 0) {
      co_return n;
    }
    if (n == 0) {
      co_return 0;  // EOF
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      auto r = co_await base_.wait_read_ready();
      if (!r) {
        // If wait returned EOF, it means the connection was closed.
        // Return 0 to indicate EOF (standard POSIX semantics).
        if (r.error() == error::eof) {
          co_return 0;
        }
        co_return unexpected(r.error());
      }
      if (!read_op_.is_epoch_current(my_epoch)) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }
    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

inline auto stream_socket_impl::async_write_some(std::span<std::byte const> buffer)
  -> awaitable<result<std::size_t>> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    co_return unexpected(error::not_open);
  }

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
    // Note: On Linux, writing to a socket whose peer has closed the connection
    // may raise SIGPIPE and terminate the process when using ::write().
    // Therefore, this implementation uses ::send(..., MSG_NOSIGNAL) instead.
    auto n = ::send(fd, buffer.data(), buffer.size(), MSG_NOSIGNAL);
    if (n >= 0) {
      // Note: write returning 0 is uncommon; treat it as a successful 0-byte write.
      co_return n;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      auto r = co_await base_.wait_write_ready();
      if (!r) {
        co_return unexpected(r.error());
      }
      if (!write_op_.is_epoch_current(my_epoch)) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }
    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

inline auto stream_socket_impl::shutdown(shutdown_type what) -> result<void> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return fail(error::not_open);
  }

  int how = SHUT_RDWR;
  if (what == shutdown_type::receive) {
    how = SHUT_RD;
  } else if (what == shutdown_type::send) {
    how = SHUT_WR;
  } else {
    how = SHUT_RDWR;
  }

  if (::shutdown(fd, how) != 0) {
    if (errno == ENOTCONN) {
      return fail(error::not_connected);
    }
    return fail(std::error_code(errno, std::generic_category()));
  }

  // Update logical shutdown state only after syscall succeeds.
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
