#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/socket_handle_base.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/error.hpp>

#include <iocoro/detail/socket/acceptor_impl.hpp>
#include <iocoro/detail/socket_endpoint_utils.hpp>
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
///   fd lifecycle and common option APIs.
/// - `iocoro::detail::socket::acceptor_impl`: protocol-agnostic acceptor implementation.
/// - Protocol semantics (endpoint conversion, socket type/protocol) are handled here in the facade.
///
/// Important:
/// - This type is protocol-typed (via `Protocol` template parameter).
/// - Protocol decides type/protocol; endpoint (or caller) decides family.
/// - `async_accept()` returns a connected `basic_stream_socket<Protocol>` and adopts the
///   accepted native fd internally.
template <class Protocol>
class basic_acceptor {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;
  using socket = basic_stream_socket<Protocol>;
  using impl_type = ::iocoro::detail::socket::acceptor_impl;
  using handle_type = ::iocoro::detail::socket_handle_base<impl_type>;

  basic_acceptor() = delete;

  explicit basic_acceptor(any_io_executor ex) : handle_(std::move(ex)) {}
  explicit basic_acceptor(io_context& ctx) : handle_(ctx) {}

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
    if (!is_open()) {
      if (auto ec = handle_.impl().open(ep.family(), Protocol::type(), Protocol::protocol())) {
        return ec;
      }
    }
    std::invoke(std::forward<Configure>(configure), *this);
    if (auto ec = handle_.impl().bind(ep.data(), ep.size())) {
      return ec;
    }
    return handle_.impl().listen(backlog);
  }

  auto local_endpoint() const -> expected<endpoint, std::error_code> {
    return ::iocoro::detail::socket::get_local_endpoint<endpoint>(handle_.native_handle());
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
    // Temporarily construct IO executor from io_context_impl to create socket.
    auto* ctx_impl = handle_.get_io_context_impl();
    socket s{any_io_executor{io_context::executor_type{*ctx_impl}}};
    if (auto ec = s.assign(*r)) {
      co_return unexpected(ec);
    }
    co_return s;
  }

  auto get_executor() const noexcept -> any_io_executor { return handle_.get_executor(); }

  auto native_handle() const noexcept -> int { return handle_.native_handle(); }

  void close() noexcept { handle_.close(); }
  auto is_open() const noexcept -> bool { return handle_.is_open(); }

  void cancel() noexcept { handle_.cancel(); }
  void cancel_read() noexcept { handle_.cancel_read(); }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return handle_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return handle_.get_option(opt);
  }

 private:
  /// Accept and return the connected native fd (low-level building block).
  auto async_accept_fd() -> awaitable<expected<int, std::error_code>> {
    co_return co_await handle_.impl().async_accept();
  }

  handle_type handle_;
};

}  // namespace iocoro::net
