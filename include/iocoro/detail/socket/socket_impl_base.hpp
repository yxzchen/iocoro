#pragma once

#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <system_error>

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
 public:
  socket_impl_base() noexcept = default;
  explicit socket_impl_base(executor ex) noexcept : ex_(ex) {}

  socket_impl_base(socket_impl_base const&) = delete;
  auto operator=(socket_impl_base const&) -> socket_impl_base& = delete;
  socket_impl_base(socket_impl_base&&) = delete;
  auto operator=(socket_impl_base&&) -> socket_impl_base& = delete;

  ~socket_impl_base() = default;

  auto get_executor() const noexcept -> executor { return ex_; }

  /// Native handle. Returns -1 if not open.
  auto native_handle() const noexcept -> int { return fd_.load(std::memory_order_acquire); }
  auto is_open() const noexcept -> bool { return native_handle() >= 0; }

  /// Open the socket resource. (Stub; returns not_implemented.)
  auto open(int /*domain*/, int /*type*/, int /*protocol*/) noexcept -> std::error_code {
    return error::not_implemented;
  }

  /// Cancel pending operations (best-effort).
  void cancel() noexcept {
    // Stub: no real registrations yet.
    // We still clear any stored handles so future implementations can be swapped in
    // without changing this interface.
    std::scoped_lock lk{mtx_};
    read_handle_ = {};
    write_handle_ = {};
  }

  /// Close the socket (best-effort, idempotent).
  void close() noexcept {
    cancel();
    fd_.store(-1, std::memory_order_release);
  }

  /// Release ownership of the native handle without closing it.
  auto release() noexcept -> int { return fd_.exchange(-1, std::memory_order_acq_rel); }

 protected:
  // Reactor registration handles for cancellation. Concrete implementations will set these.
  using fd_event_handle = io_context_impl::fd_event_handle;

  void set_read_handle(fd_event_handle h) noexcept {
    std::scoped_lock lk{mtx_};
    read_handle_ = h;
  }

  void set_write_handle(fd_event_handle h) noexcept {
    std::scoped_lock lk{mtx_};
    write_handle_ = h;
  }

  auto take_read_handle() noexcept -> fd_event_handle {
    std::scoped_lock lk{mtx_};
    return std::exchange(read_handle_, {});
  }

  auto take_write_handle() noexcept -> fd_event_handle {
    std::scoped_lock lk{mtx_};
    return std::exchange(write_handle_, {});
  }

  void set_native_handle(int fd) noexcept { fd_.store(fd, std::memory_order_release); }

 private:
  executor ex_{};
  std::atomic<int> fd_{-1};

  mutable std::mutex mtx_{};
  fd_event_handle read_handle_{};
  fd_event_handle write_handle_{};
};

}  // namespace iocoro::detail::socket
