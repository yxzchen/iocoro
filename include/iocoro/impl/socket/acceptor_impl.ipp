#include <iocoro/detail/socket/acceptor_impl.hpp>
#include <iocoro/detail/socket_utils.hpp>
#include <iocoro/detail/scope_guard.hpp>

namespace iocoro::detail::socket {

inline void acceptor_impl::cancel_read() noexcept {
  accept_op_.cancel();
  base_.cancel_read();
}

inline auto acceptor_impl::close() noexcept -> result<void> {
  {
    std::scoped_lock lk{mtx_};
    accept_op_.cancel();
    listening_ = false;
  }
  return base_.close();
}

inline auto acceptor_impl::open(int domain, int type, int protocol) -> result<void> {
  auto r = base_.open(domain, type, protocol);
  if (!r) {
    return r;
  }
  std::scoped_lock lk{mtx_};
  listening_ = false;
  return ok();
}

inline auto acceptor_impl::bind(sockaddr const* addr, socklen_t len) -> result<void> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return fail(error::not_open);
  }
  if (::bind(fd, addr, len) != 0) {
    return fail(std::error_code(errno, std::generic_category()));
  }
  return ok();
}

inline auto acceptor_impl::listen(int backlog) -> result<void> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return fail(error::not_open);
  }
  if (backlog <= 0) {
    backlog = SOMAXCONN;
  }
  if (::listen(fd, backlog) != 0) {
    return fail(std::error_code(errno, std::generic_category()));
  }
  {
    std::scoped_lock lk{mtx_};
    listening_ = true;
  }
  return ok();
}

inline auto acceptor_impl::async_accept() -> awaitable<result<int>> {
  auto const listen_fd = base_.native_handle();
  if (listen_fd < 0) {
    co_return unexpected(error::not_open);
  }

  // Single async_accept constraint: only one in-flight accept allowed.
  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (!listening_) {
      co_return unexpected(error::not_listening);
    }
    if (accept_op_.active) {
      co_return unexpected(error::busy);
    }
    accept_op_.active = true;
    my_epoch = accept_op_.epoch.load(std::memory_order_acquire);
  }

  // RAII guard to ensure accept operation is marked finished on all exit paths.
  auto guard = detail::make_scope_exit([this] { accept_op_.finish(mtx_); });

  for (;;) {
    // Cancellation check to close the "cancel between accept() and wait_read_ready()" race.
    if (!accept_op_.is_epoch_current(my_epoch)) {
      co_return unexpected(error::operation_aborted);
    }

#if defined(__linux__)
    int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd >= 0) {
      if (!set_cloexec(fd) || !set_nonblocking(fd)) {
        auto ec = std::error_code(errno, std::generic_category());
        (void)::close(fd);
        co_return unexpected(ec);
      }
    }
#endif

    if (fd >= 0) {
      if (!accept_op_.is_epoch_current(my_epoch)) {
        (void)::close(fd);
        co_return unexpected(error::operation_aborted);
      }
      co_return fd;
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (!accept_op_.is_epoch_current(my_epoch)) {
        co_return unexpected(error::operation_aborted);
      }
      auto r = co_await base_.wait_read_ready();
      if (!r) {
        co_return unexpected(r.error());
      }
      if (!accept_op_.is_epoch_current(my_epoch)) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }

    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

}  // namespace iocoro::detail::socket
