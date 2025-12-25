#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/basic_socket.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/ip/tcp/endpoint.hpp>
#include <iocoro/ip/tcp/socket.hpp>

#include <system_error>

namespace iocoro::detail::ip::tcp {
class acceptor_impl;
}

namespace iocoro::ip::tcp {

/// TCP acceptor (listening socket) with coroutine-based async accept.
///
/// Design notes (first stage):
/// - Mirrors the `tcp::socket` structure: a thin public wrapper over a `detail::*_impl`.
/// - Only interface declarations are provided here; definitions live in `.ipp`.
class acceptor : public basic_socket<detail::ip::tcp::acceptor_impl> {
 public:
  using base_type = basic_socket<detail::ip::tcp::acceptor_impl>;

  acceptor() = delete;

  explicit acceptor(executor ex);
  explicit acceptor(io_context& ctx);

  acceptor(acceptor const&) = delete;
  auto operator=(acceptor const&) -> acceptor& = delete;

  acceptor(acceptor&&) = default;
  auto operator=(acceptor&&) -> acceptor& = default;

  /// Open the underlying listening socket with the given address family (AF_INET/AF_INET6).
  auto open(int family) -> std::error_code;

  /// Bind the acceptor to the given local endpoint.
  auto bind(endpoint const& ep) -> std::error_code;

  /// Mark the acceptor as a listening socket.
  ///
  /// If `backlog` is 0, the implementation may choose a sensible default (e.g. SOMAXCONN).
  auto listen(int backlog = 0) -> std::error_code;

  /// Accept a new connection and return a connected `tcp::socket`.
  auto async_accept() -> awaitable<expected<socket, std::error_code>>;

  /// Query the local endpoint this acceptor is bound to.
  auto local_endpoint() const -> expected<endpoint, std::error_code>;

  using base_type::get_executor;
  using base_type::native_handle;

  using base_type::close;
  using base_type::is_open;

  using base_type::cancel;

  using base_type::get_option;
  using base_type::set_option;
};

}  // namespace iocoro::ip::tcp
