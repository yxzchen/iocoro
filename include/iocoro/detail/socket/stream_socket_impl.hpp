#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/operation_base.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/shutdown.hpp>

#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>

// Native socket address types (POSIX).
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::socket {

/// Stream-socket implementation shared by multiple protocols.
///
/// This layer does NOT know about ip::endpoint (or any higher-level endpoint type).
/// It accepts native `(sockaddr*, socklen_t)` views.
///
/// Concurrency:
/// - At most one in-flight read and one in-flight write are intended (full-duplex).
/// - Conflicting operations should return `error::busy` (first stage: stub).
///
/// Cancellation token contract (ties into `socket_impl_base`):
/// - This type guarantees at most one in-flight readiness waiter per direction via
///   `read_in_flight_` / `write_in_flight_`.
/// - It is therefore valid for `socket_impl_base` to store only the most-recent cancel handle
///   per direction (read/write).
class stream_socket_impl {
 public:
  stream_socket_impl() noexcept = default;
  explicit stream_socket_impl(executor ex) noexcept : base_(ex) {}

  stream_socket_impl(stream_socket_impl const&) = delete;
  auto operator=(stream_socket_impl const&) -> stream_socket_impl& = delete;
  stream_socket_impl(stream_socket_impl&&) = delete;
  auto operator=(stream_socket_impl&&) -> stream_socket_impl& = delete;

  ~stream_socket_impl() = default;

  auto get_executor() const noexcept -> executor { return base_.get_executor(); }
  auto native_handle() const noexcept -> int { return base_.native_handle(); }

  /// Open a new native socket (best-effort, non-blocking).
  ///
  /// This is a thin forwarding API to `socket_impl_base` intended for composing
  /// protocol-specific adapters (e.g. TCP) and for basic integration tests.
  ///
  /// NOTE (internal/testing):
  /// - This is NOT part of the public, user-facing networking API.
  /// - End users should prefer higher-level protocol types (e.g. `ip::tcp_socket`).
  auto open(int domain, int type, int protocol) noexcept -> std::error_code {
    return base_.open(domain, type, protocol);
  }

  /// Adopt an existing native handle (e.g. accept()).
  ///
  /// NOTE (internal/testing): helper for adapters/tests; not a public API surface.
  auto assign(int fd) noexcept -> std::error_code { return base_.assign(fd); }

  /// Release ownership of the native handle without closing it.
  ///
  /// NOTE (internal/testing): helper for adapters/tests; not a public API surface.
  auto release() noexcept -> int { return base_.release(); }

  auto is_open() const noexcept -> bool { return base_.is_open(); }
  auto is_connected() const noexcept -> bool {
    std::scoped_lock lk{mtx_};
    return (state_ == conn_state::connected);
  }

  /// Cancel pending operations (best-effort).
  ///
  /// Semantics:
  /// - Aborts waiters registered with the reactor (connect/read/write readiness waits).
  /// - Does NOT directly modify stream-level state (e.g. conn_state). The awaiting coroutines
  ///   observe cancellation via their wait result and clean up accordingly.
  /// - Does NOT reset in-flight flags here; the awaiting coroutines will clear them on resume.
  void cancel() noexcept {
    {
      std::scoped_lock lk{mtx_};
      ++epoch_;
    }
    base_.cancel();
  }

  /// Close the stream socket (best-effort, idempotent).
  ///
  /// Semantics:
  /// - Cancels and closes the underlying fd via socket_impl_base.
  /// - Resets stream-level state so the object can be reused after a later assign/open.
  void close() noexcept {
    {
      std::scoped_lock lk{mtx_};
      ++epoch_;
      state_ = conn_state::closed;
      shutdown_ = {};
      // NOTE: do not touch read_in_flight_/write_in_flight_ here; their owner is the coroutine.
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

  /// Connect to a native endpoint.
  auto async_connect(sockaddr const* addr, socklen_t len) -> awaitable<std::error_code> {
    auto const fd = base_.native_handle();
    if (fd < 0) {
      co_return error::not_open;
    }

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
      my_epoch = epoch_;
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
      state_ = conn_state::closed;
      co_return ec;
    }

    // Wait for writability, then check SO_ERROR.
    auto wait_ec = co_await wait_write_ready();
    if (wait_ec) {
      std::scoped_lock lk{mtx_};
      state_ = conn_state::closed;
      co_return wait_ec;
    }

    {
      // If cancel()/close() happened while we were waiting, treat as aborted.
      std::scoped_lock lk{mtx_};
      if (epoch_ != my_epoch) {
        state_ = conn_state::closed;
        co_return error::operation_aborted;
      }
    }

    int so_error = 0;
    socklen_t optlen = sizeof(so_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0) {
      ec = std::error_code(errno, std::generic_category());
      std::scoped_lock lk{mtx_};
      state_ = conn_state::closed;
      co_return ec;
    }
    if (so_error != 0) {
      ec = std::error_code(so_error, std::generic_category());
      std::scoped_lock lk{mtx_};
      state_ = conn_state::closed;
      co_return ec;
    }

    {
      std::scoped_lock lk{mtx_};
      if (epoch_ != my_epoch) {
        state_ = conn_state::closed;
        co_return error::operation_aborted;
      }
      state_ = conn_state::connected;
    }
    co_return std::error_code{};
  }

  /// Read at most `size` bytes into `data`.
  auto async_read_some(std::span<std::byte> buffer)
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
      my_epoch = epoch_;
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
        auto ec = co_await wait_read_ready();
        if (ec) {
          co_return unexpected(ec);
        }
        {
          std::scoped_lock lk{mtx_};
          if (epoch_ != my_epoch) {
            co_return unexpected(error::operation_aborted);
          }
        }
        continue;
      }
      co_return unexpected(std::error_code(errno, std::generic_category()));
    }
  }

  /// Write at most `size` bytes from `data`.
  auto async_write_some(std::span<std::byte const> buffer)
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
      my_epoch = epoch_;
    }

    auto guard = finally([this] {
      std::scoped_lock lk{mtx_};
      write_in_flight_ = false;
    });

    if (buffer.empty()) {
      co_return 0;
    }

    for (;;) {
      auto n = ::write(fd, buffer.data(), buffer.size());
      if (n >= 0) {
        // Note: write returning 0 is uncommon; treat it as a successful 0-byte write.
        co_return n;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        auto ec = co_await wait_write_ready();
        if (ec) {
          co_return unexpected(ec);
        }
        {
          std::scoped_lock lk{mtx_};
          if (epoch_ != my_epoch) {
            co_return unexpected(error::operation_aborted);
          }
        }
        continue;
      }
      co_return unexpected(std::error_code(errno, std::generic_category()));
    }
  }

  auto shutdown(shutdown_type what) -> std::error_code {
    auto const fd = base_.native_handle();
    if (fd < 0) return error::not_open;

    int how = SHUT_RDWR;
    if (what == shutdown_type::read) {
      how = SHUT_RD;
    } else if (what == shutdown_type::write) {
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
      if (what == shutdown_type::read) {
        shutdown_.read = true;
      } else if (what == shutdown_type::write) {
        shutdown_.write = true;
      } else {
        shutdown_.read = true;
        shutdown_.write = true;
      }
    }
    return {};
  }

 private:
  enum class conn_state : std::uint8_t { closed, connecting, connected };
  struct shutdown_state {
    bool read = false;
    bool write = false;
  };

  // Minimal scope-exit helper (no exceptions thrown from body).
  template <class F>
  class final_action {
   public:
    explicit final_action(F f) noexcept : f_(std::move(f)) {}
    final_action(final_action const&) = delete;
    auto operator=(final_action const&) -> final_action& = delete;
    final_action(final_action&&) = delete;
    auto operator=(final_action&&) -> final_action& = delete;
    ~final_action() { f_(); }

   private:
    F f_;
  };
  template <class F>
  static auto finally(F f) noexcept -> final_action<F> {
    return final_action<F>(std::move(f));
  }

  struct wait_state {
    std::coroutine_handle<> h{};
    std::error_code ec{};
    std::atomic<bool> done{false};
  };

  class fd_wait_operation final : public iocoro::detail::operation_base {
   public:
    enum class kind { read, write };

    fd_wait_operation(kind k, int fd, socket_impl_base* base, executor ex,
                      std::shared_ptr<wait_state> st) noexcept
        : operation_base(ex), kind_(k), fd_(fd), base_(base), ex_(ex), st_(std::move(st)) {}

    void execute() override { complete(std::error_code{}); }
    void abort(std::error_code ec) override { complete(ec); }

   private:
    void do_start(std::unique_ptr<iocoro::detail::operation_base> self) override {
      // Register and publish handle for cancellation.
      // Note: `socket_impl_base` retains only ONE handle per direction (the latest).
      // The surrounding `stream_socket_impl` design (in-flight flags) must maintain the
      // "single waiter per direction" invariant for correctness.
      if (kind_ == kind::read) {
        auto h = impl_->register_fd_read(fd_, std::move(self));
        if (base_ != nullptr) {
          base_->set_read_handle(h);
        }
      } else {
        auto h = impl_->register_fd_write(fd_, std::move(self));
        if (base_ != nullptr) {
          base_->set_write_handle(h);
        }
      }
    }

    void complete(std::error_code ec) {
      // Guard against double completion (execute + abort, or repeated signals).
      if (st_->done.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      st_->ec = ec;
      ex_.post([s = st_] { s->h.resume(); });
    }

    kind kind_;
    int fd_;
    socket_impl_base* base_ = nullptr;
    executor ex_{};
    std::shared_ptr<wait_state> st_{};
  };

  auto wait_read_ready() -> awaitable<std::error_code> {
    auto const fd = base_.native_handle();
    if (fd < 0) co_return error::not_open;

    auto ex = base_.get_executor();
    if (!ex) co_return error::not_open;

    // Create a shared state for completion.
    auto st = std::make_shared<wait_state>();

    struct awaiter {
      stream_socket_impl* self;
      int fd;
      executor ex;
      std::shared_ptr<wait_state> st;

      awaiter(stream_socket_impl* self_, int fd_, executor ex_, std::shared_ptr<wait_state> st_)
          : self(self_), fd(fd_), ex(ex_), st(st_) {}

      bool await_ready() const noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> h) {
        st->h = h;
        auto op = std::make_unique<fd_wait_operation>(fd_wait_operation::kind::read, fd,
                                                      &self->base_, ex, st);
        // Register immediately so cancel/close can't race ahead of registration.
        op->start(std::move(op));
        return true;
      }
      auto await_resume() noexcept -> std::error_code { return st->ec; }
    };

    co_return co_await awaiter{this, fd, ex, st};
  }

  auto wait_write_ready() -> awaitable<std::error_code> {
    auto const fd = base_.native_handle();
    if (fd < 0) co_return error::not_open;

    auto ex = base_.get_executor();
    if (!ex) co_return error::not_open;

    auto st = std::make_shared<wait_state>();

    struct awaiter {
      stream_socket_impl* self;
      int fd;
      executor ex;
      std::shared_ptr<wait_state> st;

      awaiter(stream_socket_impl* self_, int fd_, executor ex_, std::shared_ptr<wait_state> st_)
          : self(self_), fd(fd_), ex(ex_), st(st_) {}

      bool await_ready() const noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> h) {
        st->h = h;
        auto op = std::make_unique<fd_wait_operation>(fd_wait_operation::kind::write, fd,
                                                      &self->base_, ex, st);
        op->start(std::move(op));
        return true;
      }
      auto await_resume() noexcept -> std::error_code { return st->ec; }
    };

    co_return co_await awaiter{this, fd, ex, st};
  }

  socket_impl_base base_{};

  mutable std::mutex mtx_{};
  conn_state state_{conn_state::closed};
  std::uint64_t epoch_{0};
  shutdown_state shutdown_{};
  bool read_in_flight_{false};
  bool write_in_flight_{false};
  bool connect_in_flight_{false};
};

}  // namespace iocoro::detail::socket
