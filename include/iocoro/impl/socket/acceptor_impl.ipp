#include <iocoro/detail/socket/acceptor_impl.hpp>

namespace iocoro::detail::socket {

inline void acceptor_impl::cancel_read() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++accept_epoch_;
  }
  base_.cancel_read();
}

inline void acceptor_impl::close() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++accept_epoch_;
    listening_ = false;
    accept_active_ = false;
  }
  base_.close();
}

inline auto acceptor_impl::open(int domain, int type, int protocol) -> std::error_code {
  auto ec = base_.open(domain, type, protocol);
  if (ec) {
    return ec;
  }
  std::scoped_lock lk{mtx_};
  listening_ = false;
  return {};
}

inline auto acceptor_impl::bind(sockaddr const* addr, socklen_t len) -> std::error_code {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return error::not_open;
  }
  if (::bind(fd, addr, len) != 0) {
    return std::error_code(errno, std::generic_category());
  }
  return {};
}

inline auto acceptor_impl::listen(int backlog) -> std::error_code {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return error::not_open;
  }
  if (backlog <= 0) {
    backlog = SOMAXCONN;
  }
  if (::listen(fd, backlog) != 0) {
    return std::error_code(errno, std::generic_category());
  }
  {
    std::scoped_lock lk{mtx_};
    listening_ = true;
  }
  return {};
}

inline auto acceptor_impl::async_accept() -> awaitable<expected<int, std::error_code>> {
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
    if (accept_active_) {
      co_return unexpected(error::busy);
    }
    accept_active_ = true;
    my_epoch = accept_epoch_;
  }

  // RAII guard to ensure accept_active_ is cleared on all exit paths.
  struct accept_guard {
    acceptor_impl* self;
    ~accept_guard() {
      std::scoped_lock lk{self->mtx_};
      self->accept_active_ = false;
    }
  };
  accept_guard guard{this};

  for (;;) {
    // Cancellation check to close the "cancel between accept() and wait_read_ready()" race.
    {
      std::scoped_lock lk{mtx_};
      if (accept_epoch_ != my_epoch) {
        co_return unexpected(error::operation_aborted);
      }
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
      {
        std::scoped_lock lk{mtx_};
        if (accept_epoch_ != my_epoch) {
          (void)::close(fd);
          co_return unexpected(error::operation_aborted);
        }
      }
      co_return fd;
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      {
        std::scoped_lock lk{mtx_};
        if (accept_epoch_ != my_epoch) {
          co_return unexpected(error::operation_aborted);
        }
      }
      auto ec = co_await base_.wait_read_ready();
      if (ec) {
        co_return unexpected(ec);
      }
      {
        std::scoped_lock lk{mtx_};
        if (accept_epoch_ != my_epoch) {
          co_return unexpected(error::operation_aborted);
        }
      }
      continue;
    }

    co_return unexpected(std::error_code(errno, std::generic_category()));
  }
}

inline auto acceptor_impl::set_nonblocking(int fd) noexcept -> bool {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  if ((flags & O_NONBLOCK) != 0) {
    return true;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline auto acceptor_impl::set_cloexec(int fd) noexcept -> bool {
  int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  if ((flags & FD_CLOEXEC) != 0) {
    return true;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

}  // namespace iocoro::detail::socket
