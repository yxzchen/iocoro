#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/ip/tcp/endpoint.hpp>

#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <system_error>

// Native socket APIs.
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::ip::tcp {

/// TCP acceptor implementation (IP-specific adapter).
class acceptor_impl {
 public:
  acceptor_impl() noexcept = default;
  explicit acceptor_impl(executor ex) noexcept : base_(ex) {}

  acceptor_impl(acceptor_impl const&) = delete;
  auto operator=(acceptor_impl const&) -> acceptor_impl& = delete;
  acceptor_impl(acceptor_impl&&) = delete;
  auto operator=(acceptor_impl&&) -> acceptor_impl& = delete;

  ~acceptor_impl() = default;

  auto get_executor() const noexcept -> executor { return base_.get_executor(); }
  auto native_handle() const noexcept -> int { return base_.native_handle(); }
  auto is_open() const noexcept -> bool { return base_.is_open(); }

  void cancel() noexcept {
    {
      std::scoped_lock lk{mtx_};
      ++accept_epoch_;
    }
    base_.cancel();
  }

  void cancel_read() noexcept {
    {
      std::scoped_lock lk{mtx_};
      ++accept_epoch_;
    }
    base_.cancel_read();
  }

  void cancel_write() noexcept { base_.cancel_write(); }

  void close() noexcept {
    {
      std::scoped_lock lk{mtx_};
      ++accept_epoch_;
      listening_ = false;
      accept_active_ = false;
    }
    base_.close();
  }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return base_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return base_.get_option(opt);
  }

  auto open(int family) -> std::error_code {
    auto ec = base_.open(family, SOCK_STREAM, IPPROTO_TCP);
    if (ec) return ec;
    std::scoped_lock lk{mtx_};
    listening_ = false;
    return {};
  }

  auto bind(iocoro::ip::tcp::endpoint const& ep) -> std::error_code {
    auto const fd = base_.native_handle();
    if (fd < 0) {
      return error::not_open;
    }
    if (::bind(fd, ep.data(), ep.size()) != 0) {
      return std::error_code(errno, std::generic_category());
    }
    return {};
  }

  auto listen(int backlog) -> std::error_code {
    auto const fd = base_.native_handle();
    if (fd < 0) return error::not_open;
    if (backlog <= 0) backlog = SOMAXCONN;
    if (::listen(fd, backlog) != 0) {
      return std::error_code(errno, std::generic_category());
    }
    {
      std::scoped_lock lk{mtx_};
      listening_ = true;
    }
    return {};
  }

  auto local_endpoint() const -> expected<iocoro::ip::tcp::endpoint, std::error_code> {
    auto const fd = base_.native_handle();
    if (fd < 0) return unexpected(error::not_open);

    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
      return unexpected(std::error_code(errno, std::generic_category()));
    }
    return iocoro::ip::tcp::endpoint::from_native(reinterpret_cast<sockaddr*>(&ss), len);
  }

  /// Accept a new connection.
  ///
  /// Returns:
  /// - a native connected fd on success (to be adopted by a `tcp::socket` implementation)
  /// - error_code on failure
  ///
  /// NOTE (fd ownership):
  /// On success, this function returns a *native fd* that the caller MUST either adopt
  /// into a socket object or close. This is safe for the public API because `tcp::acceptor`
  /// immediately adopts the fd into a `tcp::socket` before returning to user code.
  auto async_accept() -> awaitable<expected<int, std::error_code>> {
    auto const listen_fd = base_.native_handle();
    if (listen_fd < 0) {
      co_return unexpected(error::not_open);
    }

    // Queue-based serialization:
    // - Multiple coroutines may call async_accept(); they will be served FIFO.
    // - Only the coroutine holding the "turn" is allowed to run the accept loop.
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
      // Each loop iteration begins with a cancellation check to close the "cancel between
      // accept() and wait_read_ready()" race window.
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
        // Ensure accepted sockets are non-blocking and close-on-exec on non-Linux platforms.
        // If we can't enforce non-blocking, treat it as an error to avoid surprising blocking IO.
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
        // If cancel/close happened after accept() returned EAGAIN but before we register
        // read-interest, abort now instead of potentially waiting forever.
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

 private:
  static auto set_nonblocking(int fd) noexcept -> bool {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if ((flags & O_NONBLOCK) != 0) return true;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  static auto set_cloexec(int fd) noexcept -> bool {
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0) return false;
    if ((flags & FD_CLOEXEC) != 0) return true;
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
  }

  struct accept_turn_state {
    std::coroutine_handle<> h{};
  };

  struct accept_turn_awaiter {
    acceptor_impl* self;
    std::shared_ptr<accept_turn_state> st;

    accept_turn_awaiter(acceptor_impl* self_, std::shared_ptr<accept_turn_state> st_)
        : self(self_), st(st_) {}

    bool await_ready() noexcept { return self->try_acquire_turn(st); }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
      st->h = h;
      return true;
    }

    void await_resume() noexcept {}
  };

  auto try_acquire_turn(std::shared_ptr<accept_turn_state> const& st) noexcept -> bool {
    std::scoped_lock lk{mtx_};
    cleanup_expired_queue_front();
    IOCORO_ENSURE(!accept_queue_.empty(),
                  "acceptor_impl: accept_queue_ unexpectedly empty; turn state must be queued");
    if (accept_active_) return false;

    auto front = accept_queue_.front().lock();
    IOCORO_ENSURE(static_cast<bool>(front),
                  "acceptor_impl: accept_queue_ front expired after cleanup");
    if (front.get() != st.get()) return false;
    accept_active_ = true;
    return true;
  }

  void cleanup_expired_queue_front() noexcept {
    while (!accept_queue_.empty() && accept_queue_.front().expired()) {
      accept_queue_.pop_front();
    }
  }

  void complete_turn(std::shared_ptr<accept_turn_state> const& st) noexcept {
    std::coroutine_handle<> next_h{};
    executor ex{};
    {
      std::scoped_lock lk{mtx_};

      // Remove ourselves from the queue (FIFO invariant: active turn is always at front).
      cleanup_expired_queue_front();
      IOCORO_ENSURE(!accept_queue_.empty(),
                    "acceptor_impl: completing turn but accept_queue_ is empty");
      auto front = accept_queue_.front().lock();
      IOCORO_ENSURE(static_cast<bool>(front),
                    "acceptor_impl: accept_queue_ front expired while completing turn");
      IOCORO_ENSURE(front.get() == st.get(),
                    "acceptor_impl: FIFO invariant broken; completing state is not queue front");
      accept_queue_.pop_front();

      accept_active_ = false;
      cleanup_expired_queue_front();

      if (!accept_queue_.empty()) {
        auto next = accept_queue_.front().lock();
        IOCORO_ENSURE(static_cast<bool>(next),
                      "acceptor_impl: accept_queue_ front expired unexpectedly (post-cleanup)");
        accept_active_ = true;
        next_h = next->h;
        ex = base_.get_executor();
      }
    }

    // Resume next waiter (if it actually suspended).
    if (next_h) {
      ex.post([h = next_h] { h.resume(); });
    }
  }

  template <class F>
  class final_action {
   public:
    explicit final_action(F f) noexcept : f_(std::move(f)) {}
    ~final_action() { f_(); }

   private:
    F f_;
  };
  template <class F>
  static auto finally(F f) noexcept -> final_action<F> {
    return final_action<F>(std::move(f));
  }

  socket::socket_impl_base base_{};
  mutable std::mutex mtx_{};
  bool listening_{false};
  bool accept_active_{false};
  std::uint64_t accept_epoch_{0};

  std::deque<std::weak_ptr<accept_turn_state>> accept_queue_{};
};

}  // namespace iocoro::detail::ip::tcp
