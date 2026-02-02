#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/result.hpp>

#include <iocoro/detail/socket/socket_impl_base.hpp>
#include <iocoro/detail/socket/op_state.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <system_error>

// Native socket APIs (generic / non-domain-specific).
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::socket {

/// Protocol-agnostic acceptor implementation.
///
/// Boundary:
/// - Does NOT know about endpoint types or Protocol tags.
/// - Accepts native `(sockaddr*, socklen_t)` views.
/// - Socket creation requires explicit (domain, type, protocol) parameters.
///
/// Concurrency:
/// - Multiple concurrent async_accept() calls are serialized via a FIFO queue.
/// - At most one accept operation is active at a time.
class acceptor_impl {
 public:
  acceptor_impl() noexcept = delete;
  explicit acceptor_impl(any_io_executor ex) noexcept : base_(ex) {}

  acceptor_impl(acceptor_impl const&) = delete;
  auto operator=(acceptor_impl const&) -> acceptor_impl& = delete;
  acceptor_impl(acceptor_impl&&) = delete;
  auto operator=(acceptor_impl&&) -> acceptor_impl& = delete;

  ~acceptor_impl() = default;

  auto get_io_context_impl() const noexcept -> io_context_impl* { return base_.get_io_context_impl(); }
  auto native_handle() const noexcept -> int { return base_.native_handle(); }
  auto is_open() const noexcept -> bool { return base_.is_open(); }

  void cancel() noexcept { cancel_read(); }

  void cancel_read() noexcept;

  void cancel_write() noexcept { IOCORO_UNREACHABLE(); }

  auto close() noexcept -> result<void>;

  template <class Option>
  auto set_option(Option const& opt) -> result<void> {
    return base_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> result<void> {
    return base_.get_option(opt);
  }

  /// Open a new native socket.
  auto open(int domain, int type, int protocol) -> result<void>;

  /// Bind to a native endpoint.
  auto bind(sockaddr const* addr, socklen_t len) -> result<void>;

  /// Start listening for connections.
  auto listen(int backlog) -> result<void>;

  /// Accept a new connection.
  ///
  /// Concurrency:
  /// - Only one async_accept() call is allowed at a time.
  /// - If an accept is already in progress, returns error::busy.
  ///
  /// Returns:
  /// - a native connected fd on success (to be adopted by a stream socket)
  /// - error_code on failure
  auto async_accept() -> awaitable<result<int>>;

 private:
  socket_impl_base base_;

  mutable std::mutex mtx_{};
  bool listening_{false};
  op_state accept_op_;
};

}  // namespace iocoro::detail::socket

#include <iocoro/impl/socket/acceptor_impl.ipp>
