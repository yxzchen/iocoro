#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/socket_handle_base.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>

#include <iocoro/detail/net/basic_acceptor_impl.hpp>
#include <iocoro/net/basic_stream_socket.hpp>

#include <concepts>
#include <utility>
#include <functional>
#include <system_error>

namespace iocoro::net {

/// Protocol-typed acceptor facade (network semantic layer).
///
/// This is a networking facade layered on top of:
/// - `iocoro::detail::socket_handle_base<Impl>`: a small, reusable PImpl wrapper that provides
///   fd lifecycle and common cancellation/option APIs.
/// - `iocoro::detail::net::basic_acceptor_impl<Protocol>`: protocol-injected implementation.
///
/// Important:
/// - This type is protocol-typed (via `Protocol` template parameter).
/// - Protocol decides type/protocol; endpoint (or caller) decides family.
/// - `async_accept()` returns a connected `basic_stream_socket<Protocol>` and adopts the
///   accepted native fd internally.
template <class Protocol>
class basic_acceptor : public ::iocoro::detail::socket_handle_base<
                         ::iocoro::detail::net::basic_acceptor_impl<Protocol>> {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;
  using socket = basic_stream_socket<Protocol>;
  using impl_type = ::iocoro::detail::net::basic_acceptor_impl<Protocol>;
  using base_type = ::iocoro::detail::socket_handle_base<impl_type>;

  basic_acceptor() = delete;

  explicit basic_acceptor(io_executor ex) : base_type(ex) {}
  explicit basic_acceptor(io_context& ctx) : base_type(ctx) {}

  basic_acceptor(basic_acceptor const&) = delete;
  auto operator=(basic_acceptor const&) -> basic_acceptor& = delete;

  basic_acceptor(basic_acceptor&&) = default;
  auto operator=(basic_acceptor&&) -> basic_acceptor& = default;

  /// Open + bind + listen in one step.
  ///
  /// This is the recommended user-facing entry point for acceptors.
  auto listen(endpoint const& ep, int backlog = 0) -> std::error_code {
    return listen(ep, backlog, [](basic_acceptor&) {});
  }

  /// Open + (configure) + bind + listen in one step.
  ///
  /// `configure` runs after open() succeeds and before bind() is called.
  /// This enables pre-bind socket options like SO_REUSEADDR.
  template <class Configure>
    requires std::invocable<Configure, basic_acceptor&>
  auto listen(endpoint const& ep, int backlog, Configure&& configure) -> std::error_code {
    if (!this->is_open()) {
      if (auto ec = this->impl_->open(ep.family())) {
        return ec;
      }
    }
    std::invoke(std::forward<Configure>(configure), *this);
    if (auto ec = this->impl_->bind(ep)) {
      return ec;
    }
    return this->impl_->listen(backlog);
  }

  auto local_endpoint() const -> expected<endpoint, std::error_code> {
    return this->impl_->local_endpoint();
  }

  /// Accept and return the connected native fd (low-level building block).
  auto async_accept_fd() -> awaitable<expected<int, std::error_code>> {
    co_return co_await this->impl_->async_accept();
  }

  /// Accept and return a connected `socket`.
  ///
  /// Notes:
  /// - The returned socket is bound to the same io_context as this acceptor.
  /// - The accepted native fd is adopted atomically; no fd leaks occur on failure.
  auto async_accept() -> awaitable<expected<socket, std::error_code>> {
    auto r = co_await async_accept_fd();
    if (!r) {
      co_return unexpected(r.error());
    }
    // Temporarily construct io_executor from io_context_impl to create socket.
    auto* ctx_impl = this->get_io_context_impl();
    socket s{io_executor{*ctx_impl}};
    if (auto ec = s.assign(*r)) {
      co_return unexpected(ec);
    }
    co_return s;
  }

  using base_type::native_handle;

  using base_type::close;
  using base_type::is_open;

  using base_type::cancel;
  using base_type::cancel_read;

  using base_type::get_option;
  using base_type::set_option;
};

}  // namespace iocoro::net
