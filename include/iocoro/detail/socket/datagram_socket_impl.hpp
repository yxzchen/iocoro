#pragma once

#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>

#include <iocoro/detail/scope_guard.hpp>
#include <iocoro/detail/socket/op_state.hpp>
#include <iocoro/detail/socket/socket_impl_base.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <system_error>
#include <utility>

// Native socket address types (POSIX).
#include <sys/socket.h>

namespace iocoro::detail::socket {

/// Datagram socket implementation shared by multiple protocols (e.g., UDP).
///
/// This layer does NOT know about ip::endpoint (or any higher-level endpoint type).
/// It accepts native `(sockaddr*, socklen_t)` views.
///
/// Design simplifications:
/// - Once opened, the address family is fixed (no mixing IPv4/IPv6).
/// - For connected sockets, send_to() uses send() internally (kernel handles destination).
///
/// State model:
/// - idle: socket is open but has NO local address (cannot receive).
/// - bound: socket has an EXPLICIT local address via bind() (can receive).
/// - connected: socket has an IMPLICIT local address via connect() (can receive and send to fixed peer).
///
/// Concurrency:
/// - Send and receive operations are independent (can run concurrently).
/// - At most one in-flight send and one in-flight receive are allowed.
/// - Conflicting operations return `error::busy`.
///
class datagram_socket_impl {
 public:
  datagram_socket_impl() noexcept = delete;
  explicit datagram_socket_impl(any_io_executor ex) noexcept : base_(ex) {}

  datagram_socket_impl(datagram_socket_impl const&) = delete;
  auto operator=(datagram_socket_impl const&) -> datagram_socket_impl& = delete;
  datagram_socket_impl(datagram_socket_impl&&) = delete;
  auto operator=(datagram_socket_impl&&) -> datagram_socket_impl& = delete;

  ~datagram_socket_impl() = default;

  auto get_io_context_impl() const noexcept -> io_context_impl* {
    return base_.get_io_context_impl();
  }
  auto get_executor() const noexcept -> any_io_executor { return base_.get_executor(); }
  auto native_handle() const noexcept -> int { return base_.native_handle(); }

  /// Open a new native socket (best-effort, non-blocking).
  ///
  /// NOTE: This is called internally by bind() or connect().
  /// End users should NOT call this directly; use bind() or connect() instead.
  auto open(int domain, int type, int protocol) noexcept -> std::error_code {
    return base_.open(domain, type, protocol);
  }

  auto is_open() const noexcept -> bool { return base_.is_open(); }

  auto is_bound() const noexcept -> bool {
    std::scoped_lock lk{mtx_};
    return (state_ == dgram_state::bound || state_ == dgram_state::connected);
  }

  auto is_connected() const noexcept -> bool {
    std::scoped_lock lk{mtx_};
    return (state_ == dgram_state::connected);
  }

  /// Cancel pending operations (best-effort).
  void cancel() noexcept;

  /// Cancel pending receive operations (best-effort).
  void cancel_read() noexcept;

  /// Cancel pending send operations (best-effort).
  void cancel_write() noexcept;

  /// Close the datagram socket (best-effort, idempotent).
  void close() noexcept;

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return base_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return base_.get_option(opt);
  }

  /// Bind to a local endpoint.
  auto bind(sockaddr const* addr, socklen_t len) -> std::error_code;

  /// Connect to a remote endpoint (fixes the peer for this socket).
  auto connect(sockaddr const* addr, socklen_t len) -> std::error_code;

  /// Send a datagram to the specified destination.
  ///
  /// Important:
  /// - The entire buffer is sent as a single datagram (message boundary preserved).
  /// - If the socket is connected, the destination MUST match the connected endpoint.
  /// - Returns the number of bytes sent (should equal buffer size for datagram sockets).
  auto async_send_to(
      std::span<std::byte const> buffer,
      sockaddr const* dest_addr,
      socklen_t dest_len) -> awaitable<expected<std::size_t, std::error_code>>;

  /// Receive a datagram and retrieve the source endpoint.
  ///
  /// Important:
  /// - The socket must be bound before calling this.
  /// - The entire message is received in one operation (message boundary preserved).
  /// - If the buffer is too small, an error (message_size) is returned.
  /// - src_len must be initialized to the size of the src_addr buffer before calling.
  auto async_receive_from(
      std::span<std::byte> buffer,
      sockaddr* src_addr,
      socklen_t* src_len) -> awaitable<expected<std::size_t, std::error_code>>;

 private:
  enum class dgram_state : std::uint8_t {
    idle,       // Socket opened but not bound.
    bound,      // Socket bound to a local address.
    connected   // Socket connected to a remote peer.
  };

  socket_impl_base base_;

  mutable std::mutex mtx_{};

  dgram_state state_{dgram_state::idle};
  op_state send_op_{};
  op_state receive_op_{};

  // Store the connected endpoint for validation.
  sockaddr_storage connected_addr_{};
  socklen_t connected_addr_len_{0};

};

}  // namespace iocoro::detail::socket

#include <iocoro/impl/socket/datagram_socket_impl.ipp>
