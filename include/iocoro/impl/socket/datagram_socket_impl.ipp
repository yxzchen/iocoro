#include <iocoro/detail/socket/datagram_socket_impl.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

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

inline auto datagram_socket_impl::bind(sockaddr const* addr, socklen_t len) -> result<void> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    return fail(error::not_open);
  }
  if (res->closing()) {
    return fail(error::operation_aborted);
  }

  {
    std::scoped_lock lk{mtx_};
    if (send_op_.is_active() || receive_op_.is_active()) {
      return fail(error::busy);
    }
  }

  if (::bind(res->native_handle(), addr, len) != 0) {
    return fail(map_socket_errno(errno));
  }

  {
    std::scoped_lock lk{mtx_};
    if (state_ == dgram_state::idle) {
      state_ = dgram_state::bound;
    }
  }

  return ok();
}

inline auto datagram_socket_impl::connect(sockaddr const* addr, socklen_t len) -> result<void> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    return fail(error::not_open);
  }
  if (res->closing()) {
    return fail(error::operation_aborted);
  }

  {
    std::scoped_lock lk{mtx_};
    if (send_op_.is_active() || receive_op_.is_active()) {
      return fail(error::busy);
    }
  }

  if (::connect(res->native_handle(), addr, len) != 0) {
    return fail(map_socket_errno(errno));
  }

  {
    std::scoped_lock lk{mtx_};
    state_ = dgram_state::connected;
    connected_addr_len_ = len;
    std::memcpy(&connected_addr_, addr, len);
  }

  return ok();
}

inline auto datagram_socket_impl::async_send_to(std::span<std::byte const> buffer,
                                                sockaddr const* dest_addr, socklen_t dest_len)
  -> awaitable<result<std::size_t>> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    co_return unexpected(error::not_open);
  }

  auto inflight = base_.make_operation_guard(res);
  if (!inflight) {
    co_return unexpected(error::operation_aborted);
  }
  auto const fd = res->native_handle();

  std::uint64_t my_epoch = 0;
  bool is_connected = false;
  sockaddr_storage connected_addr{};
  socklen_t connected_addr_len = 0;
  {
    std::scoped_lock lk{mtx_};
    if (!send_op_.try_start(my_epoch)) {
      co_return unexpected(error::busy);
    }
    is_connected = (state_ == dgram_state::connected);
    if (is_connected) {
      connected_addr = connected_addr_;
      connected_addr_len = connected_addr_len_;
    }
  }

  auto guard = detail::make_scope_exit([this] { send_op_.finish(); });

  if (buffer.empty()) {
    co_return 0;
  }

  if (is_connected) {
    if ((dest_addr == nullptr) != (dest_len == 0)) {
      co_return unexpected(error::invalid_argument);
    }

    auto same_destination = [](sockaddr const* a, socklen_t alen, sockaddr const* b,
                               socklen_t blen) noexcept -> bool {
      if (!a || !b || a->sa_family != b->sa_family) {
        return false;
      }
      if (a->sa_family == AF_INET) {
        if (alen < static_cast<socklen_t>(sizeof(sockaddr_in)) ||
            blen < static_cast<socklen_t>(sizeof(sockaddr_in))) {
          return false;
        }
        auto const* sa = reinterpret_cast<sockaddr_in const*>(a);
        auto const* sb = reinterpret_cast<sockaddr_in const*>(b);
        return sa->sin_port == sb->sin_port && sa->sin_addr.s_addr == sb->sin_addr.s_addr;
      }
      if (a->sa_family == AF_INET6) {
        if (alen < static_cast<socklen_t>(sizeof(sockaddr_in6)) ||
            blen < static_cast<socklen_t>(sizeof(sockaddr_in6))) {
          return false;
        }
        auto const* sa = reinterpret_cast<sockaddr_in6 const*>(a);
        auto const* sb = reinterpret_cast<sockaddr_in6 const*>(b);
        return sa->sin6_port == sb->sin6_port &&
               std::memcmp(&sa->sin6_addr, &sb->sin6_addr, sizeof(in6_addr)) == 0 &&
               sa->sin6_scope_id == sb->sin6_scope_id;
      }
      if (alen != blen) {
        return false;
      }
      return std::memcmp(a, b, static_cast<std::size_t>(alen)) == 0;
    };

    if (dest_addr) {
      if (!same_destination(dest_addr, dest_len, reinterpret_cast<sockaddr const*>(&connected_addr),
                            connected_addr_len)) {
        co_return unexpected(error::invalid_argument);
      }
    }
  }

  for (;;) {
    if (!send_op_.is_epoch_current(my_epoch) || res->closing()) {
      co_return unexpected(error::operation_aborted);
    }

    ssize_t n;
    if (is_connected) {
      n = ::send(fd, buffer.data(), buffer.size(), detail::socket::send_no_signal_flags());
    } else {
      n = ::sendto(fd, buffer.data(), buffer.size(), detail::socket::send_no_signal_flags(),
                   dest_addr, dest_len);
    }

    if (n >= 0) {
      co_return static_cast<std::size_t>(n);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      auto r = co_await base_.wait_write_ready(res);
      if (!r) {
        co_return unexpected(r.error());
      }
      if (!send_op_.is_epoch_current(my_epoch) || res->closing()) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }

    co_return unexpected(map_socket_errno(errno));
  }
}

inline auto datagram_socket_impl::async_receive_from(std::span<std::byte> buffer,
                                                     sockaddr* src_addr, socklen_t* src_len)
  -> awaitable<result<std::size_t>> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    co_return unexpected(error::not_open);
  }

  {
    std::scoped_lock lk{mtx_};
    if (state_ == dgram_state::idle) {
      co_return unexpected(error::not_bound);
    }
  }

  auto inflight = base_.make_operation_guard(res);
  if (!inflight) {
    co_return unexpected(error::operation_aborted);
  }
  auto const fd = res->native_handle();

  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (!receive_op_.try_start(my_epoch)) {
      co_return unexpected(error::busy);
    }
  }

  auto guard = detail::make_scope_exit([this] { receive_op_.finish(); });

  if (buffer.empty()) {
    co_return unexpected(error::invalid_argument);
  }

  for (;;) {
    if (!receive_op_.is_epoch_current(my_epoch) || res->closing()) {
      co_return unexpected(error::operation_aborted);
    }

    ssize_t n = ::recvfrom(fd, buffer.data(), buffer.size(), MSG_TRUNC, src_addr, src_len);
    if (n >= 0) {
      if (static_cast<std::size_t>(n) > buffer.size()) {
        co_return unexpected(error::message_size);
      }
      co_return static_cast<std::size_t>(n);
    }

    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      auto r = co_await base_.wait_read_ready(res);
      if (!r) {
        co_return unexpected(r.error());
      }
      if (!receive_op_.is_epoch_current(my_epoch) || res->closing()) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }

    co_return unexpected(map_socket_errno(errno));
  }
}

}  // namespace iocoro::detail::socket
