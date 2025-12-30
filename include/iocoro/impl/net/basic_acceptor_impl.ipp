#include <iocoro/detail/net/basic_acceptor_impl.hpp>

namespace iocoro::detail::net {

template <class Protocol>
inline void basic_acceptor_impl<Protocol>::cancel_read() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++accept_epoch_;
  }
  base_.cancel_read();
}

template <class Protocol>
inline void basic_acceptor_impl<Protocol>::close() noexcept {
  {
    std::scoped_lock lk{mtx_};
    ++accept_epoch_;
    listening_ = false;
    accept_active_ = false;
  }
  base_.close();
}

template <class Protocol>
inline auto basic_acceptor_impl<Protocol>::open(int family) -> std::error_code {
  auto ec = base_.open(family, Protocol::type(), Protocol::protocol());
  if (ec) {
    return ec;
  }
  std::scoped_lock lk{mtx_};
  listening_ = false;
  return {};
}

template <class Protocol>
inline auto basic_acceptor_impl<Protocol>::bind(endpoint_type const& ep) -> std::error_code {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return error::not_open;
  }
  if (::bind(fd, ep.data(), ep.size()) != 0) {
    return std::error_code(errno, std::generic_category());
  }
  return {};
}

template <class Protocol>
inline auto basic_acceptor_impl<Protocol>::listen(int backlog) -> std::error_code {
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

template <class Protocol>
inline auto basic_acceptor_impl<Protocol>::local_endpoint() const
  -> expected<endpoint_type, std::error_code> {
  auto const fd = base_.native_handle();
  if (fd < 0) {
    return unexpected(error::not_open);
  }

  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    return unexpected(std::error_code(errno, std::generic_category()));
  }
  return endpoint_type::from_native(reinterpret_cast<sockaddr*>(&ss), len);
}

template <class Protocol>
inline auto basic_acceptor_impl<Protocol>::async_accept() -> awaitable<expected<int, std::error_code>> {
  auto const listen_fd = base_.native_handle();
  if (listen_fd < 0) {
    co_return unexpected(error::not_open);
  }

  // Queue-based serialization (FIFO).
  auto st = std::make_shared<accept_turn_state>();
  {
    std::scoped_lock lk{mtx_};
    accept_queue_.push_back(st);
  }

  co_await accept_turn_awaiter{this, st};

  // Ensure we always release our turn and wake the next queued accept.
  auto turn_guard = finally([this, st] { complete_turn(st); });

  std::uint64_t my_epoch = 0;
  {
    std::scoped_lock lk{mtx_};
    if (!listening_) {
      co_return unexpected(error::not_listening);
    }
    my_epoch = accept_epoch_;
  }

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

template <class Protocol>
inline auto basic_acceptor_impl<Protocol>::set_nonblocking(int fd) noexcept -> bool {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  if ((flags & O_NONBLOCK) != 0) {
    return true;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

template <class Protocol>
inline auto basic_acceptor_impl<Protocol>::set_cloexec(int fd) noexcept -> bool {
  int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  if ((flags & FD_CLOEXEC) != 0) {
    return true;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

template <class Protocol>
inline auto basic_acceptor_impl<Protocol>::try_acquire_turn(
  std::shared_ptr<accept_turn_state> const& st) noexcept -> bool {
  std::scoped_lock lk{mtx_};
  cleanup_expired_queue_front();
  IOCORO_ENSURE(
    !accept_queue_.empty(),
    "basic_acceptor_impl: accept_queue_ unexpectedly empty; turn state must be queued");
  if (accept_active_) {
    return false;
  }

  auto front = accept_queue_.front().lock();
  IOCORO_ENSURE(static_cast<bool>(front),
                "basic_acceptor_impl: accept_queue_ front expired after cleanup");
  if (front.get() != st.get()) {
    return false;
  }
  accept_active_ = true;
  return true;
}

template <class Protocol>
inline void basic_acceptor_impl<Protocol>::cleanup_expired_queue_front() noexcept {
  while (!accept_queue_.empty() && accept_queue_.front().expired()) {
    accept_queue_.pop_front();
  }
}

template <class Protocol>
inline void basic_acceptor_impl<Protocol>::complete_turn(std::shared_ptr<accept_turn_state> const& st) noexcept {
  std::coroutine_handle<> next_h{};
  io_executor ex{};
  {
    std::scoped_lock lk{mtx_};

    // Remove ourselves from the queue (FIFO invariant: active turn is always at front).
    cleanup_expired_queue_front();
    IOCORO_ENSURE(!accept_queue_.empty(),
                  "basic_acceptor_impl: completing turn but accept_queue_ is empty");
    auto front = accept_queue_.front().lock();
    IOCORO_ENSURE(static_cast<bool>(front),
                  "basic_acceptor_impl: accept_queue_ front expired while completing turn");
    IOCORO_ENSURE(
      front.get() == st.get(),
      "basic_acceptor_impl: FIFO invariant broken; completing state is not queue front");
    accept_queue_.pop_front();

    accept_active_ = false;
    cleanup_expired_queue_front();

    if (!accept_queue_.empty()) {
      auto next = accept_queue_.front().lock();
      IOCORO_ENSURE(
        static_cast<bool>(next),
        "basic_acceptor_impl: accept_queue_ front expired unexpectedly (post-cleanup)");
      accept_active_ = true;
      next_h = next->h;
      ex = base_.get_executor();
    }
  }

  // Resume next waiter (if it actually suspended).
  if (next_h) {
    ex.post([h = next_h, ex] {
      detail::executor_guard g{ex};
      h.resume();
    });
  }
}

}  // namespace iocoro::detail::net


