#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/detail/socket_handle_base.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/result.hpp>

#include <iocoro/detail/socket/datagram_socket_impl.hpp>
#include <iocoro/detail/socket_utils.hpp>

#include <cstddef>
#include <span>
#include <system_error>

namespace iocoro::net {

/// Protocol-typed datagram socket facade (network semantic layer).
///
/// Layering / responsibilities:
/// - `iocoro::detail::socket_handle_base<Impl>` is a small protocol-agnostic PImpl wrapper
///   (fd lifecycle, cancel/close, socket options, native_handle).
/// - `iocoro::net::basic_datagram_socket<Protocol>` is the protocol-typed *network facade*
///   providing bind/connect/send_to/receive_from semantics.
/// - The underlying implementation is `iocoro::detail::socket::datagram_socket_impl`
///   (protocol-agnostic datagram IO).
/// - Protocol semantics (endpoint conversion, socket type/protocol) are handled here in the facade.
///
/// Construction:
/// - No default constructor: a socket must be bound to an IO executor (or io_context) up-front.
/// - Protocol is fixed by the template parameter.
///
/// Important semantics:
/// - bind() or connect() are the only points where the socket is opened.
/// - Once opened, the address family is fixed (no mixing IPv4/IPv6).
/// - For connected sockets, send_to() requires the destination to match the connected endpoint.
template <class Protocol>
class basic_datagram_socket {
 public:
  using protocol_type = Protocol;
  using endpoint_type = typename Protocol::endpoint;
  using impl_type = ::iocoro::detail::socket::datagram_socket_impl;
  using handle_type = ::iocoro::detail::socket_handle_base<impl_type>;

  basic_datagram_socket() = delete;

  explicit basic_datagram_socket(any_io_executor ex) : handle_(std::move(ex)) {}
  explicit basic_datagram_socket(io_context& ctx) : handle_(ctx) {}

  basic_datagram_socket(basic_datagram_socket const&) = delete;
  auto operator=(basic_datagram_socket const&) -> basic_datagram_socket& = delete;

  basic_datagram_socket(basic_datagram_socket&&) = default;
  auto operator=(basic_datagram_socket&&) -> basic_datagram_socket& = default;

  /// Bind the socket to a local endpoint.
  ///
  /// IMPORTANT: This is a lazy-open point. If the socket is not open, this call opens it and
  /// fixes the address family to `local_ep.family()`.
  auto bind(endpoint_type const& local_ep) -> result<void> {
    auto open_r = ok();
    if (!handle_.impl().is_open()) {
      open_r = handle_.impl().open(local_ep.family(), Protocol::type(), Protocol::protocol());
    }
    return open_r.and_then([&] { return handle_.impl().bind(local_ep.data(), local_ep.size()); });
  }

  /// Connect the socket to a remote endpoint.
  ///
  /// IMPORTANT: This is a lazy-open point. If the socket is not open, this call opens it and
  /// fixes the address family to `remote_ep.family()`.
  ///
  /// After connecting, the socket has a fixed peer; sending to other destinations is invalid.
  auto connect(endpoint_type const& remote_ep) -> result<void> {
    auto open_r = ok();
    if (!handle_.impl().is_open()) {
      open_r = handle_.impl().open(remote_ep.family(), Protocol::type(), Protocol::protocol());
    }
    return open_r.and_then(
      [&] { return handle_.impl().connect(remote_ep.data(), remote_ep.size()); });
  }

  /// Send a datagram to the specified destination.
  ///
  /// The entire buffer is sent as a single datagram (message boundary preserved).
  auto async_send_to(std::span<std::byte const> buffer, endpoint_type const& destination)
    -> awaitable<result<std::size_t>> {
    co_return co_await handle_.impl().async_send_to(buffer, destination.data(), destination.size());
  }

  /// Receive a datagram and retrieve the source endpoint.
  ///
  /// Important: The socket must be bound before calling this.
  /// The entire message is received in one operation (message boundary preserved).
  /// If the buffer is too small, an error is returned (message_size).
  auto async_receive_from(std::span<std::byte> buffer, endpoint_type& source)
    -> awaitable<result<std::size_t>> {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);

    auto result =
      co_await handle_.impl().async_receive_from(buffer, reinterpret_cast<sockaddr*>(&ss), &len);

    if (!result) {
      co_return unexpected(result.error());
    }

    auto ep_result = endpoint_type::from_native(reinterpret_cast<sockaddr*>(&ss), len);
    if (!ep_result) {
      co_return unexpected(ep_result.error());
    }

    source = *ep_result;
    co_return *result;
  }

  /// Query the local endpoint for an open socket.
  auto local_endpoint() const -> result<endpoint_type> {
    return ::iocoro::detail::socket::get_local_endpoint<endpoint_type>(handle_.native_handle());
  }

  /// Query the connected peer endpoint.
  ///
  /// Returns `error::not_open` if the socket is not open and `error::not_connected` if it is
  /// open but not connected.
  auto remote_endpoint() const -> result<endpoint_type> {
    auto const fd = handle_.native_handle();
    if (fd < 0) {
      return unexpected(error::not_open);
    }
    if (!handle_.impl().is_connected()) {
      return unexpected(error::not_connected);
    }
    return ::iocoro::detail::socket::get_remote_endpoint<endpoint_type>(fd);
  }

  auto is_bound() const noexcept -> bool { return handle_.impl().is_bound(); }
  auto is_connected() const noexcept -> bool { return handle_.impl().is_connected(); }

  auto get_executor() const noexcept -> any_io_executor { return handle_.get_executor(); }

  auto native_handle() const noexcept -> int { return handle_.native_handle(); }

  auto close() noexcept -> result<void> { return handle_.close(); }
  auto is_open() const noexcept -> bool { return handle_.is_open(); }

  void cancel() noexcept { handle_.cancel(); }
  void cancel_read() noexcept { handle_.cancel_read(); }
  void cancel_write() noexcept { handle_.cancel_write(); }

  template <class Option>
  auto set_option(Option const& opt) -> result<void> {
    return handle_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> result<void> {
    return handle_.get_option(opt);
  }

 private:
  handle_type handle_;
};

}  // namespace iocoro::net
