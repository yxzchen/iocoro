#include <iocoro/detail/socket/stream_socket_impl.hpp>

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::socket {

inline void stream_socket_impl::cancel() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++read_epoch_;
    ++write_epoch_;
    ++connect_epoch_;
  }
  base_.cancel();
}

inline void stream_socket_impl::cancel_read() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++read_epoch_;
  }
  base_.cancel_read();
}

inline void stream_socket_impl::cancel_write() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++write_epoch_;
  }
  base_.cancel_write();
}

inline void stream_socket_impl::close() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++read_epoch_;
    ++write_epoch_;
    ++connect_epoch_;
    state_ = conn_state::disconnected;
    shutdown_ = {};
    // NOTE: do not touch read_in_flight_/write_in_flight_ here; their owner is the coroutine.
  }
  base_.close();
}

inline auto stream_socket_impl::bind(sockaddr const* addr, socklen_t len) -> std::error_code {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return error::not_open;
  }
  if (::bind(fd, addr, len) != 0) {
    return std::error_code(errno, std::generic_category());
  }
  return {};
}

inline auto stream_socket_impl::async_connect(sockaddr const* addr, socklen_t len)
  -> awaitable<std::error_code> {
  if (!is_open()) {
    co_return error::not_open;
  }

  auto const fd = base_.native_handle();

  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (connect_in_flight_) {
      co_return error::busy;
    }
    if (state_ == conn_state::connecting) {
      co_return error::busy;
    }
    if (state_ == conn_state::connected) {
      co_return error::already_connected;
    }
    connect_in_flight_ = true;
    state_ = conn_state::connecting;
    my_epoch = connect_epoch_;
  }

  // Ensure the "connect owner" flag is always released by the owning coroutine.
  auto connect_guard = finally([this] {
    std::scoped_lock lk{mtx_};
    connect_in_flight_ = false;
  });

  // We intentionally keep syscall logic outside the mutex.
  auto ec = std::error_code{};

  // Attempt immediate connect.
  for (;;) {
    if (::connect(fd, addr, len) == 0) {
      std::scoped_lock lk{mtx_};
      state_ = conn_state::connected;
      co_return std::error_code{};
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
    co_return ec;
  }

  // Wait for writability, then check SO_ERROR.
  auto wait_ec = co_await base_.wait_write_ready();
  if (wait_ec) {
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return wait_ec;
  }

  {
    // If cancel()/close() happened while we were waiting, treat as aborted.
    std::scoped_lock lk{mtx_};
    if (connect_epoch_ != my_epoch) {
      state_ = conn_state::disconnected;
      co_return error::operation_aborted;
    }
  }

  int so_error = 0;
  socklen_t optlen = sizeof(so_error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0) {
    ec = std::error_code(errno, std::generic_category());
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return ec;
  }
  if (so_error != 0) {
    ec = std::error_code(so_error, std::generic_category());
    std::scoped_lock lk{mtx_};
    state_ = conn_state::disconnected;
    co_return ec;
  }

  {
    std::scoped_lock lk{mtx_};
    if (connect_epoch_ != my_epoch) {
      state_ = conn_state::disconnected;
      co_return error::operation_aborted;
    }
    state_ = conn_state::connected;
  }
  co_return std::error_code{};
}

inline auto stream_socket_impl::async_read_some(std::span<std::byte> buffer)
  -> awaitable<expected<std::size_t, std::error_code>> {
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
    if (read_in_flight_) {
      co_return unexpected(error::busy);
    }
    read_in_flight_ = true;
    my_epoch = read_epoch_;
  }

  auto guard = finally([this] {
    std::scoped_lock lk{mtx_};
    read_in_flight_ = false;
  });

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
      auto ec = co_await base_.wait_read_ready();
      if (ec) {
        co_return unexpected(ec);
      }
      {
        std::scoped_lock lk{mtx_};
        if (read_epoch_ != my_epoch) {
          co_return unexpected(error::operation_aborted);
        }
      }
      continue;
    }
    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

inline auto stream_socket_impl::async_write_some(std::span<std::byte const> buffer)
  -> awaitable<expected<std::size_t, std::error_code>> {
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
    if (write_in_flight_) {
      co_return unexpected(error::busy);
    }
    write_in_flight_ = true;
    my_epoch = write_epoch_;
  }

  auto guard = finally([this] {
    std::scoped_lock lk{mtx_};
    write_in_flight_ = false;
  });

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
      auto ec = co_await base_.wait_write_ready();
      if (ec) {
        co_return unexpected(ec);
      }
      {
        std::scoped_lock lk{mtx_};
        if (write_epoch_ != my_epoch) {
          co_return unexpected(error::operation_aborted);
        }
      }
      continue;
    }
    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

inline auto stream_socket_impl::shutdown(shutdown_type what) -> std::error_code {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return error::not_open;
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
      return error::not_connected;
    }
    return std::error_code(errno, std::generic_category());
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
  return {};
}

}  // namespace iocoro::detail::socket
