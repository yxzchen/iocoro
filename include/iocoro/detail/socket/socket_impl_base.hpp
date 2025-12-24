#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_base.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/socket_option.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::socket {

/// Base class for socket-like implementations.
///
/// Responsibilities:
/// - Own the native handle (fd) lifecycle (open/close/release).
/// - Own executor binding (used to register reactor ops / post completions).
/// - Provide thread-safe cancel/close primitives.
///
/// Concurrency contract (minimal; enforced by derived classes):
/// - `cancel()` and `close()` are thread-safe and may be called from any thread.
/// - Starting async operations (read/write/connect/...) must either be internally serialized
///   or return `error::busy` when conflicting ops are in-flight.
class socket_impl_base {
 private:
  enum class fd_wait_kind : std::uint8_t { read, write };

  template <fd_wait_kind Kind>
  struct fd_awaiter;

 public:
  using fd_event_handle = io_context_impl::fd_event_handle;

  /// Socket resource lifecycle state (fd-level).
  ///
  /// Design intent:
  /// - Lifecycle operations are mutex-serialized for internal state/handle bookkeeping.
  /// - The mutex is NOT held across external/system boundaries (reactor calls, ::close, etc.).
  /// - I/O operations are handled in higher layers with a lightweight path.
  ///
  /// Note: this state is intentionally minimal and protocol-agnostic. Protocol semantics
  /// (connecting/connected/shutdown state/etc.) belong in higher-level implementations.
  enum class state : std::uint8_t { closed, opening, open };

  socket_impl_base() noexcept = default;
  explicit socket_impl_base(executor ex) noexcept : ex_(ex) {}

  socket_impl_base(socket_impl_base const&) = delete;
  auto operator=(socket_impl_base const&) -> socket_impl_base& = delete;
  socket_impl_base(socket_impl_base&&) = delete;
  auto operator=(socket_impl_base&&) -> socket_impl_base& = delete;

  ~socket_impl_base() { close(); }

  auto get_executor() const noexcept -> executor { return ex_; }

  auto get_state() const noexcept -> state {
    std::scoped_lock lk{mtx_};
    return state_;
  }

  /// Native handle snapshot. Returns -1 if not open.
  ///
  /// IMPORTANT:
  /// - This is a snapshot that may become invalid immediately after return.
  /// - The atomic is used to avoid data races for lock-free reads; it does not guarantee
  ///   any consistency beyond that.
  auto native_handle() const noexcept -> int { return fd_.load(std::memory_order_acquire); }

  /// Returns true if the socket is in the 'open' state and has a valid fd.
  /// Returns false during 'opening' (fd not yet assigned) and 'closed' states.
  auto is_open() const noexcept -> bool { return native_handle() >= 0; }

  /// Open a new socket and set it non-blocking + close-on-exec (best-effort).
  ///
  /// Returns:
  /// - {} on success
  /// - error::busy if already open
  /// - std::error_code(errno, generic_category()) for sys failures
  auto open(int domain, int type, int protocol) noexcept -> std::error_code {
    {
      std::scoped_lock lk{mtx_};
      if (state_ != state::closed || native_handle() >= 0) {
        return error::busy;
      }
      state_ = state::opening;
    }

    int fd = ::socket(domain, type, protocol);
    if (fd < 0) {
      std::scoped_lock lk{mtx_};
      if (state_ == state::opening) {
        state_ = state::closed;
      }
      return std::error_code(errno, std::generic_category());
    }

    // Best-effort: set CLOEXEC + non-blocking.
    (void)set_cloexec(fd);
    (void)set_nonblocking(fd);

    {
      std::scoped_lock lk{mtx_};
      if (state_ == state::opening) {
        fd_.store(fd, std::memory_order_release);
        state_ = state::open;
        return {};
      }
      // Aborted by close()/assign() while opening.
      // We intentionally do not adopt the fd.
    }

    (void)::close(fd);
    return error::busy;
  }

  /// Adopt an existing native handle (e.g. accept()).
  ///
  /// Note: cancels/clears any pending registrations currently stored in this object.
  auto assign(int fd) noexcept -> std::error_code {
    if (fd < 0) return error::invalid_argument;

    int old_fd = -1;
    fd_event_handle rh{};
    fd_event_handle wh{};
    {
      std::scoped_lock lk{mtx_};
      // Mark as opening to block concurrent open/assign.
      // Clear current resources (if any); close happens outside the lock.
      if (state_ == state::open) {
        old_fd = fd_.exchange(-1, std::memory_order_acq_rel);
        rh = std::exchange(read_handle_, {});
        wh = std::exchange(write_handle_, {});
      }
      state_ = state::opening;
    }

    // Cancel/deregister/close old fd outside lock.
    rh.cancel();
    wh.cancel();
    if (ex_.impl_ != nullptr && old_fd >= 0) {
      ex_.impl_->deregister_fd(old_fd);
    }
    if (old_fd >= 0) {
      (void)::close(old_fd);
    }

    // Best-effort flags.
    (void)set_cloexec(fd);
    (void)set_nonblocking(fd);

    {
      std::scoped_lock lk{mtx_};
      if (state_ == state::opening) {
        fd_.store(fd, std::memory_order_release);
        state_ = state::open;
        return {};
      }
    }

    // Aborted by close() while assigning.
    (void)::close(fd);
    return error::busy;
  }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    auto const fd = native_handle();
    if (fd < 0) return error::not_open;

    if (::setsockopt(fd, opt.level(), opt.name(), opt.data(), opt.size()) != 0) {
      return std::error_code(errno, std::generic_category());
    }
    return {};
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    auto const fd = native_handle();
    if (fd < 0) return error::not_open;

    socklen_t len = opt.size();
    if (::getsockopt(fd, opt.level(), opt.name(), opt.data(), &len) != 0) {
      return std::error_code(errno, std::generic_category());
    }
    return {};
  }

  /// Cancel any in-flight read or write operations.
  void cancel() noexcept {
    fd_event_handle rh{};
    fd_event_handle wh{};
    {
      std::scoped_lock lk{mtx_};
      rh = std::exchange(read_handle_, {});
      wh = std::exchange(write_handle_, {});
    }

    rh.cancel();
    wh.cancel();

    /// The fd_event_handle::cancel() method will handle deregistration from the IO loop
    /// if no other operations remain, so explicit deregistration here is unnecessary.
  }

  /// Cancel pending read-side operations (best-effort).
  ///
  /// Semantics:
  /// - Cancels the currently stored read-side registration handle (if any).
  /// - Does NOT affect write-side operations.
  void cancel_read() noexcept {
    fd_event_handle rh{};
    {
      std::scoped_lock lk{mtx_};
      rh = std::exchange(read_handle_, {});
    }
    rh.cancel();
  }

  /// Cancel pending write-side operations (best-effort).
  ///
  /// Semantics:
  /// - Cancels the currently stored write-side registration handle (if any).
  /// - Does NOT affect read-side operations.
  void cancel_write() noexcept {
    fd_event_handle wh{};
    {
      std::scoped_lock lk{mtx_};
      wh = std::exchange(write_handle_, {});
    }
    wh.cancel();
  }

  /// Close the socket (best-effort, idempotent).
  void close() noexcept {
    int fd = -1;
    fd_event_handle rh{};
    fd_event_handle wh{};
    {
      std::scoped_lock lk{mtx_};
      if (state_ == state::closed) {
        return;
      }

      // If opening, we only mark closed; the opener will close the fd it created.
      if (state_ == state::opening) {
        state_ = state::closed;
        read_handle_ = {};
        write_handle_ = {};
        fd_.store(-1, std::memory_order_release);
        return;
      }

      // open -> closed
      state_ = state::closed;
      fd = fd_.exchange(-1, std::memory_order_acq_rel);
      rh = std::exchange(read_handle_, {});
      wh = std::exchange(write_handle_, {});
    }

    rh.cancel();
    wh.cancel();
    if (ex_.impl_ != nullptr && fd >= 0) {
      ex_.impl_->deregister_fd(fd);
    }
    if (fd >= 0) {
      (void)::close(fd);
    }
  }

  auto has_pending_operations() const noexcept -> bool {
    std::scoped_lock lk{mtx_};
    return read_handle_.valid() || write_handle_.valid();
  }

  /// Release ownership of the native handle without closing it.
  ///
  /// IMPORTANT: This deregisters the fd from the reactor, so any pending operations will be
  /// cancelled. The caller is responsible for managing the returned fd, including closing it.
  auto release() noexcept -> int {
    int fd = -1;
    fd_event_handle rh{};
    fd_event_handle wh{};
    {
      std::scoped_lock lk{mtx_};
      fd = fd_.exchange(-1, std::memory_order_acq_rel);
      state_ = state::closed;
      rh = std::exchange(read_handle_, {});
      wh = std::exchange(write_handle_, {});
    }

    // Cancel any in-flight ops and deregister interest, but do NOT close the fd.
    rh.cancel();
    wh.cancel();
    if (ex_.impl_ != nullptr && fd >= 0) {
      ex_.impl_->deregister_fd(fd);
    }
    return fd;
  }

  /// Publish the current cancellation handle for the "read readiness" waiter.
  ///
  /// Ownership / contract:
  /// - `socket_impl_base` stores exactly ONE handle per direction (read/write).
  /// - Each call overwrites the previous handle; only the most-recent handle is retained.
  /// - `cancel()` / `close()` / `release()` will atomically `exchange()` the stored handle
  ///   and call `handle.cancel()` outside the lock.
  ///
  /// Therefore, higher layers MUST enforce that, per-direction, there is at most one
  /// in-flight waiter that relies on this handle for cancellation.
  void set_read_handle(fd_event_handle h) noexcept {
    std::scoped_lock lk{mtx_};
    read_handle_ = h;
  }

  /// Publish the current cancellation handle for the "write readiness" waiter.
  /// See `set_read_handle()` for the ownership/contract details.
  void set_write_handle(fd_event_handle h) noexcept {
    std::scoped_lock lk{mtx_};
    write_handle_ = h;
  }

  /// Wait until the native fd becomes readable (read readiness).
  ///
  /// Note: Returns a custom awaiter to avoid creating an extra coroutine frame.
  auto wait_read_ready() -> fd_awaiter<fd_wait_kind::read>;

  /// Wait until the native fd becomes writable (write readiness).
  ///
  /// Note: Returns a custom awaiter to avoid creating an extra coroutine frame.
  auto wait_write_ready() -> fd_awaiter<fd_wait_kind::write>;

 private:
  struct wait_state {
    std::coroutine_handle<> h{};
    std::error_code ec{};
    std::atomic<bool> done{false};
  };

  class fd_wait_operation final : public iocoro::detail::operation_base {
   public:
    fd_wait_operation(fd_wait_kind k, int fd, socket_impl_base* base, executor ex,
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
      if (kind_ == fd_wait_kind::read) {
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

    fd_wait_kind kind_;
    int fd_;
    socket_impl_base* base_ = nullptr;
    executor ex_{};
    std::shared_ptr<wait_state> st_{};
  };

  template <fd_wait_kind Kind>
  struct fd_awaiter {
    socket_impl_base* self;
    int fd;
    executor ex;
    std::shared_ptr<wait_state> st;

    fd_awaiter(socket_impl_base* self_, int fd_, executor ex_,
               std::shared_ptr<wait_state> st_) noexcept
        : self(self_), fd(fd_), ex(ex_), st(st_) {}

    bool await_ready() const noexcept { return fd < 0 || !ex; }

    bool await_suspend(std::coroutine_handle<> h) {
      st->h = h;
      auto op = std::make_unique<fd_wait_operation>(Kind, fd, self, ex, st);
      op->start(std::move(op));
      return true;
    }

    auto await_resume() noexcept -> std::error_code {
      if (fd < 0) return error::not_open;
      if (!ex) return error::not_open;
      return st->ec;
    }
  };

  // No locked helpers that call external/system boundaries; we keep those outside locks.

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

  executor ex_{};
  // fd_ is written under mtx_ in lifecycle operations; native_handle() reads a race-free snapshot.
  std::atomic<int> fd_{-1};
  state state_{state::closed};

  mutable std::mutex mtx_{};
  fd_event_handle read_handle_{};
  fd_event_handle write_handle_{};
};

inline auto socket_impl_base::wait_read_ready() -> fd_awaiter<fd_wait_kind::read> {
  return {this, native_handle(), get_executor(), std::make_shared<wait_state>()};
}

inline auto socket_impl_base::wait_write_ready() -> fd_awaiter<fd_wait_kind::write> {
  return {this, native_handle(), get_executor(), std::make_shared<wait_state>()};
}

}  // namespace iocoro::detail::socket
