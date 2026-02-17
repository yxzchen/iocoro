#include <iocoro/detail/scope_guard.hpp>
#include <iocoro/detail/socket/acceptor_impl.hpp>
#include <iocoro/detail/socket_utils.hpp>

namespace iocoro::detail::socket {

inline void acceptor_impl::cancel_read() noexcept {
  {
    std::scoped_lock lk{mtx_};
    accept_op_.cancel();
  }
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

inline auto acceptor_impl::listen(int backlog) -> result<void> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    return fail(error::not_open);
  }
  if (res->closing()) {
    return fail(error::operation_aborted);
  }

  if (backlog <= 0) {
    backlog = SOMAXCONN;
  }
  if (::listen(res->native_handle(), backlog) != 0) {
    return fail(map_socket_errno(errno));
  }
  {
    std::scoped_lock lk{mtx_};
    listening_ = true;
  }
  return ok();
}

inline auto acceptor_impl::async_accept() -> awaitable<result<int>> {
  auto res = base_.acquire_resource();
  if (!res || res->native_handle() < 0) {
    co_return unexpected(error::not_open);
  }

  auto inflight = base_.make_operation_guard(res);
  if (!inflight) {
    co_return unexpected(error::operation_aborted);
  }

  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (!listening_) {
      co_return unexpected(error::not_listening);
    }
    if (!accept_op_.try_start(my_epoch)) {
      co_return unexpected(error::busy);
    }
  }

  auto guard = detail::make_scope_exit([this] { accept_op_.finish(); });

  for (;;) {
    if (!accept_op_.is_epoch_current(my_epoch) || res->closing()) {
      co_return unexpected(error::operation_aborted);
    }

#if defined(__linux__)
    int fd = ::accept4(res->native_handle(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0 && errno == ENOSYS) {
      fd = ::accept(res->native_handle(), nullptr, nullptr);
      if (fd >= 0) {
        if (!set_cloexec(fd) || !set_nonblocking(fd)) {
          auto ec = map_socket_errno(errno);
          (void)::close(fd);
          co_return unexpected(ec);
        }
      }
    }
#else
    int fd = ::accept(res->native_handle(), nullptr, nullptr);
    if (fd >= 0) {
      if (!set_cloexec(fd) || !set_nonblocking(fd)) {
        auto ec = map_socket_errno(errno);
        (void)::close(fd);
        co_return unexpected(ec);
      }
    }
#endif

    if (fd >= 0) {
      if (!accept_op_.is_epoch_current(my_epoch) || res->closing()) {
        (void)::close(fd);
        co_return unexpected(error::operation_aborted);
      }
      co_return fd;
    }

    if (errno == EINTR) {
      continue;
    }

    if (is_accept_transient_error(errno)) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (!accept_op_.is_epoch_current(my_epoch) || res->closing()) {
        co_return unexpected(error::operation_aborted);
      }
      auto wait = co_await base_.wait_read_ready(res);
      if (!wait) {
        co_return unexpected(wait.error());
      }
      if (!accept_op_.is_epoch_current(my_epoch) || res->closing()) {
        co_return unexpected(error::operation_aborted);
      }
      continue;
    }

    co_return unexpected(map_socket_errno(errno));
  }
}

}  // namespace iocoro::detail::socket
