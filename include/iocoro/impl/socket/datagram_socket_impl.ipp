#include <iocoro/detail/socket/datagram_socket_impl.hpp>

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::socket {

inline void datagram_socket_impl::cancel() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++send_epoch_;
    ++receive_epoch_;
  }
  base_.cancel();
}

inline void datagram_socket_impl::cancel_read() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++receive_epoch_;
  }
  base_.cancel_read();
}

inline void datagram_socket_impl::cancel_write() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++send_epoch_;
  }
  base_.cancel_write();
}

inline void datagram_socket_impl::close() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++send_epoch_;
    ++receive_epoch_;
    state_ = dgram_state::idle;
    connected_addr_len_ = 0;
    std::memset(&connected_addr_, 0, sizeof(connected_addr_));
    // NOTE: do not touch send_in_flight_/receive_in_flight_ here; their owner is the coroutine.
  }
  base_.close();
}

inline auto datagram_socket_impl::bind(sockaddr const* addr, socklen_t len)
    -> std::error_code {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return error::not_open;
  }

  if (::bind(fd, addr, len) != 0) {
    return std::error_code(errno, std::generic_category());
  }

  {
    std::scoped_lock lk{mtx_};
    // Transition to bound state only if not already connected.
    if (state_ == dgram_state::idle) {
      state_ = dgram_state::bound;
    }
  }

  return {};
}

inline auto datagram_socket_impl::connect(sockaddr const* addr, socklen_t len)
    -> std::error_code {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return error::not_open;
  }

  // For datagram sockets, connect() is synchronous (just sets the default peer).
  if (::connect(fd, addr, len) != 0) {
    return std::error_code(errno, std::generic_category());
  }

  {
    std::scoped_lock lk{mtx_};
    state_ = dgram_state::connected;

    // Store the connected address for later validation in send_to.
    connected_addr_len_ = len;
    std::memcpy(&connected_addr_, addr, len);
  }

  return {};
}

inline auto datagram_socket_impl::async_send_to(
    std::span<std::byte const> buffer,
    sockaddr const* dest_addr,
    socklen_t dest_len,
    cancellation_token tok) -> awaitable<expected<std::size_t, std::error_code>> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    co_return unexpected(error::not_open);
  }

  std::uint64_t my_epoch = 0;
  bool is_connected = false;

  {
    std::scoped_lock lk{mtx_};
    if (send_in_flight_) {
      co_return unexpected(error::busy);
    }
    send_in_flight_ = true;
    my_epoch = send_epoch_;
    is_connected = (state_ == dgram_state::connected);
  }

  auto guard = finally([this] {
    std::scoped_lock lk{mtx_};
    send_in_flight_ = false;
  });

  if (tok.stop_requested()) {
    co_return unexpected(error::operation_aborted);
  }

  if (buffer.empty()) {
    co_return 0;
  }

  // Retry loop for EINTR and EAGAIN.
  for (;;) {
    ssize_t n;

    if (is_connected) {
      // For connected UDP sockets, use send() instead of sendto().
      // Linux behavior:
      //   - sendto(fd, ..., dest) with dest != nullptr requires dest to match the connected peer,
      //     otherwise returns EINVAL.
      //   - send(fd, ...) or sendto(fd, ..., nullptr, 0) always works correctly.
      // We use send() for clarity and correctness.
      n = ::send(fd, buffer.data(), buffer.size(), MSG_NOSIGNAL);
    } else {
      // For unconnected sockets, use sendto() with explicit destination.
      n = ::sendto(fd, buffer.data(), buffer.size(), MSG_NOSIGNAL, dest_addr, dest_len);
    }

    if (n >= 0) {
      co_return static_cast<std::size_t>(n);
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Wait for write readiness.
      auto ec = co_await base_.wait_write_ready(tok);
      if (ec) {
        co_return unexpected(ec);
      }

      // Check for cancellation.
      {
        std::scoped_lock lk{mtx_};
        if (send_epoch_ != my_epoch) {
          co_return unexpected(error::operation_aborted);
        }
      }
      continue;
    }

    // Map EMSGSIZE to our error code.
    if (errno == EMSGSIZE) {
      co_return unexpected(error::message_size);
    }

    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

inline auto datagram_socket_impl::async_receive_from(
    std::span<std::byte> buffer,
    sockaddr* src_addr,
    socklen_t* src_len,
    cancellation_token tok) -> awaitable<expected<std::size_t, std::error_code>> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    co_return unexpected(error::not_open);
  }

  // Check that the socket has a local address (required for receiving).
  //
  // State semantics:
  //   - idle: socket is open but has NO local address → cannot receive
  //   - bound: socket has an EXPLICIT local address (via bind()) → can receive
  //   - connected: socket has an IMPLICIT local address (kernel-assigned via connect()) → can receive
  //
  // Terminology note:
  //   We use "has local address" instead of "is bound" to clarify that both
  //   explicit bind() and implicit connect() satisfy the requirement.
  {
    std::scoped_lock lk{mtx_};
    if (state_ == dgram_state::idle) {
      co_return unexpected(error::not_bound);
    }
  }

  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (receive_in_flight_) {
      co_return unexpected(error::busy);
    }
    receive_in_flight_ = true;
    my_epoch = receive_epoch_;
  }

  auto guard = finally([this] {
    std::scoped_lock lk{mtx_};
    receive_in_flight_ = false;
  });

  if (tok.stop_requested()) {
    co_return unexpected(error::operation_aborted);
  }

  if (buffer.empty()) {
    co_return unexpected(error::invalid_argument);
  }

  // CRITICAL: src_len must be initialized by the caller to the size of src_addr buffer.
  // After recvfrom, it will be updated to the actual address length.

  // Retry loop for EINTR and EAGAIN.
  for (;;) {
    // Use MSG_TRUNC to detect truncation.
    //
    // Portability note (Linux-specific behavior):
    //   On Linux, MSG_TRUNC causes recvfrom to return the actual datagram size,
    //   even if it exceeds the buffer size. This allows us to detect truncation.
    //
    //   BSD systems also support MSG_TRUNC, but the exact semantics may vary slightly.
    //   POSIX does not mandate MSG_TRUNC, so strictly portable code should handle
    //   its absence (though it is widely available on modern UNIX-like systems).
    //
    //   This implementation assumes Linux-like MSG_TRUNC semantics.
    ssize_t n = ::recvfrom(fd, buffer.data(), buffer.size(), MSG_TRUNC, src_addr, src_len);

    if (n >= 0) {
      // Check if the message was truncated (return value > buffer size).
      if (static_cast<std::size_t>(n) > buffer.size()) {
        co_return unexpected(error::message_size);
      }
      co_return static_cast<std::size_t>(n);
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Wait for read readiness.
      auto ec = co_await base_.wait_read_ready(tok);
      if (ec) {
        co_return unexpected(ec);
      }

      // Check for cancellation.
      {
        std::scoped_lock lk{mtx_};
        if (receive_epoch_ != my_epoch) {
          co_return unexpected(error::operation_aborted);
        }
      }
      continue;
    }

    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

}  // namespace iocoro::detail::socket
