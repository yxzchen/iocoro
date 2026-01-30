#pragma once

#include <stop_token>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/shutdown.hpp>
#include <iocoro/io_context.hpp>

#include <iocoro/detail/socket/socket_impl_base.hpp>

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
/// Cancellation token contract (ties into `socket_impl_base`):
/// - This type guarantees at most one in-flight readiness waiter per direction via
///   `read_in_flight_` / `write_in_flight_`.
/// - It is therefore valid for `socket_impl_base` to store only the most-recent cancel handle
///   per direction (read/write).
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
  auto open(int domain, int type, int protocol) noexcept -> std::error_code {
    return base_.open(domain, type, protocol);
  }

  /// Adopt an existing native handle (e.g. accept()).
  ///
  /// INTENDED USE (acceptor pattern only):
  /// - This method is specifically designed for use by `acceptor` classes (e.g., `tcp::acceptor`)
  ///   to transfer ownership of an accepted connection fd to a new socket object.
  /// - The accepted fd represents an already-established connection (e.g., from `::accept()`).
  ///
  /// PRECONDITIONS (CRITICAL - UNDEFINED BEHAVIOR IF VIOLATED):
  /// - The target `stream_socket_impl` object MUST be default-constructed (empty state).
  /// - Calling `assign()` on a socket that has been used (via `open()`, previous `assign()`,
  ///   or any I/O operations) results in UNDEFINED BEHAVIOR.
  /// - The provided `fd` must be a valid, open file descriptor representing a connected stream
  ///   socket.
  ///
  /// POSTCONDITIONS (on success):
  /// - The socket takes ownership of `fd` and is ready for I/O operations.
  /// - The fd is set to non-blocking mode (best-effort).
  auto assign(int fd) noexcept -> std::error_code {
    IOCORO_ASSERT(state_ == conn_state::disconnected);
    IOCORO_ASSERT(!read_in_flight_ && !write_in_flight_ && !connect_in_flight_);

    auto ec = base_.assign(fd);
    if (ec) {
      return ec;
    }
    // An fd returned by accept() represents an already-established connection.
    // Mark the logical stream state as connected so read/write/remote_endpoint work.
    {
      std::scoped_lock lk{mtx_};
      state_ = conn_state::connected;
      shutdown_ = {};
    }
    return {};
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
  /// - Does NOT directly modify stream-level state (e.g. conn_state). The awaiting coroutines
  ///   observe cancellation via their wait result and clean up accordingly.
  /// - Does NOT reset in-flight flags here; the awaiting coroutines will clear them on resume.
  void cancel() noexcept;

  /// Cancel pending read-side operations (best-effort).
  ///
  /// Semantics:
  /// - Aborts the currently-registered "read readiness" waiter (if any).
  /// - Does NOT affect write-side operations.
  void cancel_read() noexcept;

  /// Cancel pending write-side operations (best-effort).
  ///
  /// Semantics:
  /// - Aborts the currently-registered "write readiness" waiter (if any).
  /// - Does NOT affect read-side operations.
  ///
  /// Note: connect() readiness waits are implemented via writability. Therefore, cancel_write()
  /// may also abort an in-flight async_connect() if it is currently waiting for writability.
  void cancel_write() noexcept;

  /// Cancel pending connect operations (best-effort).
  ///
  /// Notes:
  /// - connect readiness waits are implemented via writability.
  /// - This increments connect_epoch_ so the connect coroutine can reliably detect cancellation
  ///   even if the reactor handle was not yet published at the time of cancellation.
  void cancel_connect() noexcept;

  /// Close the stream socket (best-effort, idempotent).
  ///
  /// Semantics:
  /// - Cancels and closes the underlying fd via socket_impl_base.
  /// - Resets stream-level state so the object can be reused after a later assign/open.
  void close() noexcept;

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return base_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return base_.get_option(opt);
  }

  /// Bind to a native endpoint.
  auto bind(sockaddr const* addr, socklen_t len) -> std::error_code;

  /// Connect to a native endpoint.
  auto async_connect(sockaddr const* addr, socklen_t len) -> awaitable<std::error_code>;

  /// Read at most `size` bytes into `data`.
  auto async_read_some(std::span<std::byte> buffer)
    -> awaitable<expected<std::size_t, std::error_code>>;

  /// Write at most `size` bytes from `data`.
  auto async_write_some(std::span<std::byte const> buffer)
    -> awaitable<expected<std::size_t, std::error_code>>;

  auto shutdown(shutdown_type what) -> std::error_code;

 private:
  enum class conn_state : std::uint8_t { disconnected, connecting, connected };

  struct shutdown_state {
    bool read = false;
    bool write = false;
  };

  // Minimal scope-exit helper (no exceptions thrown from body).
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

  socket_impl_base base_;

  mutable std::mutex mtx_{};

  conn_state state_{conn_state::disconnected};
  std::uint64_t read_epoch_{0};
  std::uint64_t write_epoch_{0};
  std::uint64_t connect_epoch_{0};
  shutdown_state shutdown_{};
  bool read_in_flight_{false};
  bool write_in_flight_{false};
  bool connect_in_flight_{false};
};

}  // namespace iocoro::detail::socket

#include <iocoro/impl/socket/stream_socket_impl.ipp>
