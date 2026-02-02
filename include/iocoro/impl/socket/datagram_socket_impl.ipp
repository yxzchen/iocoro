#include <iocoro/detail/socket/datagram_socket_impl.hpp>

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::socket {

inline void datagram_socket_impl::cancel() noexcept {
  send_op_.cancel();
  receive_op_.cancel();
  base_.cancel();
}

inline void datagram_socket_impl::cancel_read() noexcept {
  receive_op_.cancel();
  base_.cancel_read();
}

inline void datagram_socket_impl::cancel_write() noexcept {
  send_op_.cancel();
  base_.cancel_write();
}

inline auto datagram_socket_impl::close() noexcept -> result<void> {
  {
    std::scoped_lock lk{mtx_};
    send_op_.cancel();
    receive_op_.cancel();
    state_ = dgram_state::idle;
    connected_addr_len_ = 0;
    std::memset(&connected_addr_, 0, sizeof(connected_addr_));
    // NOTE: do not touch active flags here; their owner is the coroutine.
  }
  return base_.close();
}

inline auto datagram_socket_impl::bind(sockaddr const* addr, socklen_t len)
    -> result<void> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return fail(error::not_open);
  }

  if (::bind(fd, addr, len) != 0) {
    return fail(std::error_code(errno, std::generic_category()));
  }

  {
    std::scoped_lock lk{mtx_};
    // Transition to bound state only if not already connected.
    if (state_ == dgram_state::idle) {
      state_ = dgram_state::bound;
    }
  }

  return ok();
}

inline auto datagram_socket_impl::connect(sockaddr const* addr, socklen_t len)
    -> result<void> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return fail(error::not_open);
  }

  // For datagram sockets, connect() is synchronous (just sets the default peer).
  if (::connect(fd, addr, len) != 0) {
    return fail(std::error_code(errno, std::generic_category()));
  }

  {
    std::scoped_lock lk{mtx_};
    state_ = dgram_state::connected;

    // Store the connected address for later validation in send_to.
    connected_addr_len_ = len;
    std::memcpy(&connected_addr_, addr, len);
  }

  return ok();
}

inline auto datagram_socket_impl::async_send_to(
    std::span<std::byte const> buffer,
    sockaddr const* dest_addr,
    socklen_t dest_len) -> awaitable<result<std::size_t>> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    co_return unexpected(error::not_open);
  }

  std::uint64_t my_epoch = 0;
  bool is_connected = false;

  {
    std::scoped_lock lk{mtx_};
    if (send_op_.active) {
      co_return unexpected(error::busy);
    }
    send_op_.active = true;
    my_epoch = send_op_.epoch.load(std::memory_order_acquire);
    is_connected = (state_ == dgram_state::connected);
  }

  auto guard = detail::make_scope_exit([this] { send_op_.finish(mtx_); });

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
      auto r = co_await base_.wait_write_ready();
      if (!r) {
        co_return unexpected(r.error());
      }
      if (!send_op_.is_epoch_current(my_epoch)) {
        co_return unexpected(error::operation_aborted);
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
    socklen_t* src_len) -> awaitable<result<std::size_t>> {
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
    if (receive_op_.active) {
      co_return unexpected(error::busy);
    }
    receive_op_.active = true;
    my_epoch = receive_op_.epoch.load(std::memory_order_acquire);
  }

  auto guard = detail::make_scope_exit([this] { receive_op_.finish(mtx_); });

  if (buffer.empty()) {
    co_return unexpected(error::invalid_argument);
  }

  // IMPORTANT: `recvfrom()` expects `*src_len` to be initialized to the size of the `src_addr`
  // buffer; it is updated on success to the actual address length.

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
      auto r = co_await base_.wait_read_ready();
      if (!r) {
        co_return unexpected(r.error());
      }
      if (!receive_op_.is_epoch_current(my_epoch)) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }

    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

}  // namespace iocoro::detail::socket
