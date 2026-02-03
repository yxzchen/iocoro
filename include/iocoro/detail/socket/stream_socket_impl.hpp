#pragma once

#include <iocoro/error.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/result.hpp>
#include <iocoro/shutdown.hpp>

#include <iocoro/detail/scope_guard.hpp>
#include <iocoro/detail/socket/op_state.hpp>
#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <system_error>
#include <utility>

// Native socket address types (POSIX).
#include <sys/socket.h>

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
class stream_socket_impl {
 public:
  stream_socket_impl() noexcept = delete;
  explicit stream_socket_impl(any_io_executor ex) noexcept : base_(ex) {}

  stream_socket_impl(stream_socket_impl const&) = delete;
  auto operator=(stream_socket_impl const&) -> stream_socket_impl& = delete;
  stream_socket_impl(stream_socket_impl&&) = delete;
  auto operator=(stream_socket_impl&&) -> stream_socket_impl& = delete;

  ~stream_socket_impl() = default;

  auto get_io_context_impl() const noexcept -> io_context_impl* {
    return base_.get_io_context_impl();
  }
  auto get_executor() const noexcept -> any_io_executor { return base_.get_executor(); }
  auto native_handle() const noexcept -> int { return base_.native_handle(); }

  /// Open a new native socket (best-effort, non-blocking).
  ///
  /// This is a thin forwarding API to `socket_impl_base` intended for composing
  /// protocol-specific adapters (e.g. TCP) and for basic integration tests.
  ///
  /// NOTE (internal/testing):
  /// - This is NOT part of the public, user-facing networking API.
  /// - End users should prefer higher-level protocol types (e.g. `ip::tcp::socket`).
  auto open(int domain, int type, int protocol) noexcept -> result<void> {
    return base_.open(domain, type, protocol);
  }

  /// Adopt an existing native handle (e.g. accept()).
  ///
  /// IMPORTANT: Intended for acceptor adoption only (transferring ownership of an accepted fd).
  ///
  /// Preconditions:
  /// - The socket must be in an unused state (no prior `open()`/`assign()` and no in-flight ops).
  /// - `fd` must refer to a connected stream socket.
  ///
  /// Postconditions on success:
  /// - This socket takes ownership of `fd` and is ready for I/O.
  /// - The fd is set to non-blocking mode (best-effort).
  auto assign(int fd) noexcept -> result<void> {
    IOCORO_ASSERT(state_ == conn_state::disconnected);
    IOCORO_ASSERT(!read_op_.active && !write_op_.active && !connect_op_.active);

    auto r = base_.assign(fd);
    if (!r) {
      return r;
    }
    // An fd returned by accept() represents an already-established connection.
    // Mark the logical stream state as connected so read/write/remote_endpoint work.
    {
      std::scoped_lock lk{mtx_};
      state_ = conn_state::connected;
      shutdown_ = {};
    }
    return ok();
  }

  auto is_open() const noexcept -> bool { return base_.is_open(); }
  auto is_connected() const noexcept -> bool {
    std::scoped_lock lk{mtx_};
    return (state_ == conn_state::connected);
  }

  /// Cancel pending operations (best-effort).
  ///
  /// Semantics:
  /// - Aborts waiters registered with the reactor (connect/read/write readiness waits).
  /// - Does NOT directly modify stream-level state (e.g. conn_state).
  void cancel() noexcept;

  /// Cancel pending read-side operations (best-effort).
  void cancel_read() noexcept;

  /// Cancel pending write-side operations (best-effort).
  ///
  /// Note: connect() readiness waits are implemented via writability. Therefore, cancel_write()
  /// may also abort an in-flight async_connect() if it is currently waiting for writability.
  void cancel_write() noexcept;

  /// Cancel pending connect operations (best-effort).
  void cancel_connect() noexcept;

  /// Close the stream socket (best-effort, idempotent).
  ///
  /// Semantics:
  /// - Cancels all pending operations (increments epochs to signal cancellation).
  /// - Closes the underlying fd via socket_impl_base.
  /// - Resets stream-level state so the object can be reused after a later assign/open.
  ///
  /// IMPORTANT - Asynchronous Close Behavior:
  /// - close() does NOT wait for pending operations to complete.
  /// - Pending operations may still be accessing the fd when it is closed.
  /// - This can result in operations receiving EBADF or other errors.
  ///
  /// Best Practices:
  /// - Ensure no operations are in-flight before calling close().
  /// - Or, use stop_token to cancel operations and wait for them to complete.
  /// - For graceful shutdown, call shutdown() before close() to signal peer.
  auto close() noexcept -> result<void>;

  template <class Option>
  auto set_option(Option const& opt) -> result<void> {
    return base_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> result<void> {
    return base_.get_option(opt);
  }

  /// Bind to a native endpoint.
  auto bind(sockaddr const* addr, socklen_t len) -> result<void>;

  /// Connect to a native endpoint.
  auto async_connect(sockaddr const* addr, socklen_t len) -> awaitable<result<void>>;

  /// Read at most `size` bytes into `data`.
  auto async_read_some(std::span<std::byte> buffer) -> awaitable<result<std::size_t>>;

  /// Write at most `size` bytes from `data`.
  auto async_write_some(std::span<std::byte const> buffer) -> awaitable<result<std::size_t>>;

  auto shutdown(shutdown_type what) -> result<void>;

 private:
  enum class conn_state : std::uint8_t { disconnected, connecting, connected };

  struct shutdown_state {
    bool read = false;
    bool write = false;
  };

  socket_impl_base base_;

  mutable std::mutex mtx_{};

  conn_state state_{conn_state::disconnected};
  op_state read_op_{};
  op_state write_op_{};
  op_state connect_op_{};
  shutdown_state shutdown_{};
};

}  // namespace iocoro::detail::socket

#include <iocoro/impl/socket/stream_socket_impl.ipp>
