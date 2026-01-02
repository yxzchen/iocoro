#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_async.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_executor.hpp>
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
/// - Own io_executor binding (used to register reactor ops / post completions).
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

  socket_impl_base() noexcept = delete;
  explicit socket_impl_base(io_executor ex) noexcept : ctx_impl_(ex.impl_) {}

  socket_impl_base(socket_impl_base const&) = delete;
  auto operator=(socket_impl_base const&) -> socket_impl_base& = delete;
  socket_impl_base(socket_impl_base&&) = delete;
  auto operator=(socket_impl_base&&) -> socket_impl_base& = delete;

  ~socket_impl_base() { close(); }

  auto get_io_context_impl() const noexcept -> io_context_impl* { return ctx_impl_; }

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
  void close() noexcept;

  auto has_pending_operations() const noexcept -> bool {
    std::scoped_lock lk{mtx_};
    return read_handle_.valid() || write_handle_.valid();
  }

  /// Release ownership of the native handle without closing it.
  ///
  /// IMPORTANT: This deregisters the fd from the reactor, so any pending operations will be
  /// cancelled. The caller is responsible for managing the returned fd, including closing it.
  auto release() noexcept -> int;

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
  auto wait_read_ready() -> awaitable<std::error_code> {
    if (native_handle() < 0) {
      co_return error::not_open;
    }
    co_return co_await operation_awaiter<fd_wait_operation<fd_wait_kind::read>>{this};
  }

  /// Wait until the native fd becomes writable (write readiness).
  auto wait_write_ready() -> awaitable<std::error_code> {
    if (native_handle() < 0) {
      co_return error::not_open;
    }
    co_return co_await operation_awaiter<fd_wait_operation<fd_wait_kind::write>>{this};
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

  template <fd_wait_kind Kind>
  class fd_wait_operation final : public async_operation {
   public:
    fd_wait_operation(std::shared_ptr<operation_wait_state> st, socket_impl_base* socket) noexcept
        : async_operation(std::move(st)), socket_(socket) {}

   private:
    void do_start(std::unique_ptr<operation_base> self) override {
      // Register and publish handle for cancellation.
      // Note: `socket_impl_base` retains only ONE handle per direction (the latest).
      // The surrounding `stream_socket_impl` design (in-flight flags) must maintain the
      // "single waiter per direction" invariant for correctness.
      if constexpr (Kind == fd_wait_kind::read) {
        auto h = socket_->ctx_impl_->register_fd_read(socket_->native_handle(), std::move(self));
        socket_->set_read_handle(h);
      } else {
        auto h = socket_->ctx_impl_->register_fd_write(socket_->native_handle(), std::move(self));
        socket_->set_write_handle(h);
      }
    }

    socket_impl_base* socket_ = nullptr;
  };

  io_context_impl* ctx_impl_{};

  mutable std::mutex mtx_{};

  // fd_ is written under mtx_ in lifecycle operations; native_handle() reads a race-free snapshot.
  std::atomic<int> fd_{-1};
  fd_state state_{fd_state::closed};

  fd_event_handle read_handle_{};
  fd_event_handle write_handle_{};
};

}  // namespace iocoro::detail::socket

#include <iocoro/impl/socket/socket_impl_base.ipp>
