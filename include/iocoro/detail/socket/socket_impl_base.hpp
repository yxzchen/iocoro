#pragma once

#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/error.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/socket_option.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <utility>

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

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
  /// Socket resource lifecycle state (fd-level).
  ///
  /// Design intent (per project rule):
  /// - Lifecycle operations (open / close / assign / release / cancel) are fully mutex-serialized.
  /// - I/O operations are handled in higher layers with a lightweight path.
  ///
  /// Note: this state is intentionally minimal and protocol-agnostic. Protocol semantics
  /// (connecting/connected/shutdown state/etc.) belong in higher-level implementations.
  enum class state : std::uint8_t { closed, open, closing };

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
  auto is_closing() const noexcept -> bool { return get_state() == state::closing; }

  /// Native handle. Returns -1 if not open.
  auto native_handle() const noexcept -> int { return fd_.load(std::memory_order_acquire); }
  auto is_open() const noexcept -> bool { return native_handle() >= 0; }

  /// Open a new socket and set it non-blocking + close-on-exec (best-effort).
  ///
  /// Returns:
  /// - {} on success
  /// - error::busy if already open
  /// - std::error_code(errno, generic_category()) for sys failures
  auto open(int domain, int type, int protocol) noexcept -> std::error_code {
    std::scoped_lock lk{mtx_};
    if (state_ != state::closed || native_handle() >= 0) {
      return error::busy;
    }

    int fd = ::socket(domain, type, protocol);
    if (fd < 0) {
      return std::error_code(errno, std::generic_category());
    }

    // Best-effort: set CLOEXEC + non-blocking.
    (void)set_cloexec(fd);
    (void)set_nonblocking(fd);

    fd_.store(fd, std::memory_order_release);
    state_ = state::open;
    return {};
  }

  /// Adopt an existing native handle (e.g. accept()).
  ///
  /// Note: cancels/clears any pending registrations currently stored in this object.
  auto assign(int fd) noexcept -> std::error_code {
    if (fd < 0) return error::invalid_argument;

    std::scoped_lock lk{mtx_};
    // Close existing state first (idempotent).
    do_close_locked();

    // Best-effort flags.
    (void)set_cloexec(fd);
    (void)set_nonblocking(fd);

    fd_.store(fd, std::memory_order_release);
    state_ = state::open;
    return {};
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

  /// Cancel pending operations (best-effort).
  void cancel() noexcept {
    std::scoped_lock lk{mtx_};
    auto const fd = native_handle();
    do_cancel_locked(fd);
  }

  /// Close the socket (best-effort, idempotent).
  void close() noexcept {
    std::scoped_lock lk{mtx_};
    do_close_locked();
  }

  /// Release ownership of the native handle without closing it.
  ///
  /// IMPORTANT: This deregisters the fd from the reactor, so any pending operations will be
  /// cancelled. The caller is responsible for managing the returned fd, including closing it.
  auto release() noexcept -> int {
    std::scoped_lock lk{mtx_};
    auto const fd = fd_.exchange(-1, std::memory_order_acq_rel);
    state_ = state::closed;
    read_handle_ = {};
    write_handle_ = {};

    if (ex_.impl_ != nullptr && fd >= 0) {
      ex_.impl_->deregister_fd(fd);
    }
    return fd;
  }

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
  void do_cancel_locked(int fd) noexcept {
    fd_event_handle rh{};
    fd_event_handle wh{};
    rh = std::exchange(read_handle_, {});
    wh = std::exchange(write_handle_, {});

    // Best-effort: cancel the specific registrations (token-based).
    rh.cancel();
    wh.cancel();

    // Best-effort: remove interest and abort registered ops for this fd.
    // Assumes io_context_impl::deregister_fd is thread-safe and idempotent.
    if (ex_.impl_ != nullptr && fd >= 0) {
      ex_.impl_->deregister_fd(fd);
    }
  }

  void do_close_locked() noexcept {
    if (state_ == state::closed) {
      fd_.store(-1, std::memory_order_release);
      return;
    }
    state_ = state::closing;

    auto const fd = fd_.exchange(-1, std::memory_order_acq_rel);

    // Cancel & deregister interest first (best-effort).
    do_cancel_locked(fd);

    if (fd >= 0) {
      (void)::close(fd);
    }

    state_ = state::closed;
  }

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
  // fd_ is written under mtx_ in lifecycle operations; native_handle() reads a snapshot.
  std::atomic<int> fd_{-1};
  state state_{state::closed};

  mutable std::mutex mtx_{};
  fd_event_handle read_handle_{};
  fd_event_handle write_handle_{};
};

}  // namespace iocoro::detail::socket
