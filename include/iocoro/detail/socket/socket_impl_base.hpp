#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_awaiter.hpp>
#include <iocoro/error.hpp>
#include <iocoro/any_executor.hpp>
#include <iocoro/socket_option.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

#include <sys/socket.h>
#include <cerrno>

namespace iocoro::detail::socket {

/// Base class for socket-like implementations.
///
/// Responsibilities:
/// - Own the native handle (fd) lifecycle (open/close/release).
/// - Own IO executor binding (used to register reactor ops / post completions).
/// - Provide thread-safe cancel/close primitives.
///
/// Concurrency contract (minimal; enforced by derived classes):
/// - `cancel()` and `close()` are thread-safe and may be called from any thread.
/// - Starting async operations (read/write/connect/...) must either be internally serialized
///   or return `error::busy` when conflicting ops are in-flight.
class socket_impl_base {
 public:
  using event_handle = io_context_impl::event_handle;

  struct fd_handle {
    int fd = -1;
    std::uint64_t gen = 0;
    explicit operator bool() const noexcept { return fd >= 0 && gen != 0; }
  };

  socket_impl_base() noexcept = delete;
  explicit socket_impl_base(any_io_executor ex) noexcept
      : ex_(std::move(ex)), ctx_impl_(ex_.io_context_ptr()) {
    IOCORO_ENSURE(ex_, "socket_impl_base: requires IO executor");
    IOCORO_ENSURE(ctx_impl_, "socket_impl_base: requires IO executor");
  }

  socket_impl_base(socket_impl_base const&) = delete;
  auto operator=(socket_impl_base const&) -> socket_impl_base& = delete;
  socket_impl_base(socket_impl_base&&) = delete;
  auto operator=(socket_impl_base&&) -> socket_impl_base& = delete;

  ~socket_impl_base() { close(); }

  auto get_io_context_impl() const noexcept -> io_context_impl* { return ctx_impl_; }
  auto get_executor() const noexcept -> any_io_executor { return ex_; }

  /// Native handle snapshot. Returns -1 if not open.
  ///
  /// IMPORTANT:
  /// - This is a snapshot that may become invalid immediately after return.
  /// - The atomic is used to avoid data races for lock-free reads; it does not guarantee
  ///   any consistency beyond that.
  auto native_handle() const noexcept -> int { return fd_.load(std::memory_order_acquire); }
  auto native_handle_gen() const noexcept -> std::uint64_t {
    return fd_gen_.load(std::memory_order_acquire);
  }

  /// Acquire a stable (fd, generation) snapshot.
  /// The generation increments whenever the underlying fd instance changes
  /// (open/assign/close/release).
  auto acquire_fd_handle() const noexcept -> fd_handle {
    return fd_handle{native_handle(), native_handle_gen()};
  }

  /// Returns true if the socket is in the 'open' state and has a valid fd.
  /// Returns false during 'opening' (fd not yet assigned) and 'closed' states.
  auto is_open() const noexcept -> bool { return native_handle() >= 0; }

  /// Open a new socket and set it non-blocking + close-on-exec (best-effort).
  ///
  /// Returns:
  /// - {} on success
  /// - error::busy if already open
  /// - std::error_code(errno, generic_category()) for sys failures
  auto open(int domain, int type, int protocol) noexcept -> std::error_code;

  /// Adopt an existing native handle (e.g. accept()).
  ///
  /// Note: cancels/clears any pending registrations currently stored in this object.
  auto assign(int fd) noexcept -> std::error_code;

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    auto const fd = native_handle();
    if (fd < 0) {
      return error::not_open;
    }

    if (::setsockopt(fd, opt.level(), opt.name(), opt.data(), opt.size()) != 0) {
      return std::error_code(errno, std::generic_category());
    }
    return {};
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    auto const fd = native_handle();
    if (fd < 0) {
      return error::not_open;
    }

    socklen_t len = opt.size();
    if (::getsockopt(fd, opt.level(), opt.name(), opt.data(), &len) != 0) {
      return std::error_code(errno, std::generic_category());
    }
    return {};
  }

  /// Cancel any in-flight read or write operations.
  void cancel() noexcept;

  /// Cancel pending read-side operations (best-effort).
  ///
  /// Semantics:
  /// - Cancels the currently stored read-side registration handle (if any).
  /// - Does NOT affect write-side operations.
  void cancel_read() noexcept;

  /// Cancel pending write-side operations (best-effort).
  ///
  /// Semantics:
  /// - Cancels the currently stored write-side registration handle (if any).
  /// - Does NOT affect read-side operations.
  void cancel_write() noexcept;

  /// Close the socket (best-effort, idempotent).
  auto close() noexcept -> std::error_code;

  auto has_pending_operations() const noexcept -> bool {
    std::scoped_lock lk{mtx_};
    return read_handle_.valid() || write_handle_.valid();
  }

  /// Release ownership of the native handle without closing it.
  ///
  /// IMPORTANT: This deregisters the fd from the reactor, so any pending operations will be
  /// cancelled. The caller is responsible for managing the returned fd, including closing it.
  auto release() noexcept -> int;

  /// Publish the current handle for the "read readiness" waiter.
  ///
  /// Ownership / contract:
  /// - `socket_impl_base` stores exactly ONE handle per direction (read/write).
  /// - Each call overwrites the previous handle; only the most-recent handle is retained.
  /// - `cancel()` / `close()` / `release()` will atomically `exchange()` the stored handle
  ///   and call `handle.cancel()` outside the lock.
  ///
  /// Therefore, higher layers MUST enforce that, per-direction, there is at most one
  /// in-flight waiter that relies on this handle for cancellation.
  void set_read_handle(fd_handle fh, event_handle h) noexcept {
    bool accept = false;
    {
      std::scoped_lock lk{mtx_};
      // Only publish the handle if the fd instance hasn't changed.
      if (state_ == fd_state::open && fh.fd == native_handle() && fh.gen == native_handle_gen()) {
        read_handle_ = h;
        accept = true;
      }
    }
    if (!accept && h) {
      h.cancel();
    }
  }

  /// Publish the current handle for the "write readiness" waiter.
  /// See `set_read_handle()` for the ownership/contract details.
  void set_write_handle(fd_handle fh, event_handle h) noexcept {
    bool accept = false;
    {
      std::scoped_lock lk{mtx_};
      if (state_ == fd_state::open && fh.fd == native_handle() && fh.gen == native_handle_gen()) {
        write_handle_ = h;
        accept = true;
      }
    }
    if (!accept && h) {
      h.cancel();
    }
  }

  /// Wait until the native fd becomes readable (read readiness).
  auto wait_read_ready() -> awaitable<std::error_code> {
    auto fh = acquire_fd_handle();
    if (!fh) {
      co_return error::not_open;
    }
    co_return co_await detail::operation_awaiter{
      [this, fh](detail::reactor_op_ptr rop) mutable {
        auto h = ctx_impl_->register_fd_read(fh.fd, std::move(rop));
        set_read_handle(fh, h);
        return h;
      }};
  }

  /// Wait until the native fd becomes writable (write readiness).
  auto wait_write_ready() -> awaitable<std::error_code> {
    auto fh = acquire_fd_handle();
    if (!fh) {
      co_return error::not_open;
    }
    co_return co_await detail::operation_awaiter{
      [this, fh](detail::reactor_op_ptr rop) mutable {
        auto h = ctx_impl_->register_fd_write(fh.fd, std::move(rop));
        set_write_handle(fh, h);
        return h;
      }};
  }

 private:
  /// Socket resource lifecycle state (fd-level).
  ///
  /// Design intent:
  /// - Lifecycle operations are mutex-serialized for internal state/handle bookkeeping.
  /// - The mutex is NOT held across external/system boundaries (reactor calls, ::close, etc.).
  /// - I/O operations are handled in higher layers with a lightweight path.
  ///
  /// Note: this state is intentionally minimal and protocol-agnostic. Protocol semantics
  /// (connecting/connected/shutdown state/etc.) belong in higher-level implementations.
  enum class fd_state : std::uint8_t { closed, opening, open };

  any_io_executor ex_{};
  io_context_impl* ctx_impl_{};

  mutable std::mutex mtx_{};

  // fd_ is written under mtx_ in lifecycle operations; native_handle() reads a race-free snapshot.
  std::atomic<int> fd_{-1};
  // Generation counter for the currently installed fd instance.
  // Incremented whenever the underlying fd identity changes.
  std::atomic<std::uint64_t> fd_gen_{1};
  fd_state state_{fd_state::closed};

  event_handle read_handle_{};
  event_handle write_handle_{};
};

}  // namespace iocoro::detail::socket

#include <iocoro/impl/socket/socket_impl_base.ipp>
