#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/socket_handle_base.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/error.hpp>

#include <iocoro/detail/socket/datagram_socket_impl.hpp>
#include <iocoro/detail/socket_endpoint_utils.hpp>

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
class basic_datagram_socket
    : public ::iocoro::detail::socket_handle_base<::iocoro::detail::socket::datagram_socket_impl> {
 public:
  using protocol_type = Protocol;
  using endpoint_type = typename Protocol::endpoint;
  using impl_type = ::iocoro::detail::socket::datagram_socket_impl;
  using base_type = ::iocoro::detail::socket_handle_base<impl_type>;

  basic_datagram_socket() = delete;

  explicit basic_datagram_socket(any_io_executor ex) : base_type(ex) {}
  explicit basic_datagram_socket(io_context& ctx) : base_type(ctx) {}

  basic_datagram_socket(basic_datagram_socket const&) = delete;
  auto operator=(basic_datagram_socket const&) -> basic_datagram_socket& = delete;

  basic_datagram_socket(basic_datagram_socket&&) = default;
  auto operator=(basic_datagram_socket&&) -> basic_datagram_socket& = default;

  /// Bind the socket to a local endpoint.
  /// This opens the socket if not already open.
  /// Must be called before receiving data.
  auto bind(endpoint_type const& local_ep) -> std::error_code {
    // Open if not already open.
    if (!this->impl_->is_open()) {
      auto ec = this->impl_->open(local_ep.family(), Protocol::type(), Protocol::protocol());
      if (ec) {
        return ec;
      }
    }
    return this->impl_->bind(local_ep.data(), local_ep.size());
  }

  /// Connect the socket to a remote endpoint.
  /// This opens the socket if not already open and fixes the remote peer.
  /// After connecting, only send_to() to the connected endpoint is allowed.
  auto connect(endpoint_type const& remote_ep) -> std::error_code {
    // Open if not already open.
    if (!this->impl_->is_open()) {
      auto ec = this->impl_->open(remote_ep.family(), Protocol::type(), Protocol::protocol());
      if (ec) {
        return ec;
      }
    }
    return this->impl_->connect(remote_ep.data(), remote_ep.size());
  }

  /// Send a datagram to the specified destination.
  ///
  /// The entire buffer is sent as a single datagram (message boundary preserved).
  auto async_send_to(std::span<std::byte const> buffer, endpoint_type const& destination)
      -> awaitable<expected<std::size_t, std::error_code>> {
    co_return co_await this->impl_->async_send_to(buffer, destination.data(), destination.size());
  }

  /// Receive a datagram and retrieve the source endpoint.
  ///
  /// Important: The socket must be bound before calling this.
  /// The entire message is received in one operation (message boundary preserved).
  /// If the buffer is too small, an error is returned (message_size).
  auto async_receive_from(std::span<std::byte> buffer, endpoint_type& source)
      -> awaitable<expected<std::size_t, std::error_code>> {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);

    auto result =
      co_await this->impl_->async_receive_from(buffer, reinterpret_cast<sockaddr*>(&ss), &len);

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

  /// Get the local endpoint.
  auto local_endpoint() const -> expected<endpoint_type, std::error_code> {
    return ::iocoro::detail::socket::get_local_endpoint<endpoint_type>(this->impl_->native_handle());
  }

  /// Get the remote endpoint (only valid if connected).
  auto remote_endpoint() const -> expected<endpoint_type, std::error_code> {
    auto const fd = this->impl_->native_handle();
    if (fd < 0) {
      return unexpected(error::not_open);
    }
    if (!this->impl_->is_connected()) {
      return unexpected(error::not_connected);
    }
    return ::iocoro::detail::socket::get_remote_endpoint<endpoint_type>(fd);
  }

  auto is_bound() const noexcept -> bool { return this->impl_->is_bound(); }
  auto is_connected() const noexcept -> bool { return this->impl_->is_connected(); }

  using base_type::native_handle;

  using base_type::close;
  using base_type::is_open;

  using base_type::cancel;
  using base_type::cancel_read;
  using base_type::cancel_write;

  using base_type::get_option;
  using base_type::set_option;
};

}  // namespace iocoro::net
