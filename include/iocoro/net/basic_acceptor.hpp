#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/socket_handle_base.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>

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
  auto listen(endpoint const& ep, int backlog = 0) -> result<void> {
    return listen(ep, backlog, [](basic_acceptor&) {});
  }

  /// Open + (configure) + bind + listen in one step.
  ///
  /// `configure` runs after open() succeeds and before bind() is called.
  /// This enables pre-bind socket options like SO_REUSEADDR.
  template <class Configure>
    requires std::invocable<Configure, basic_acceptor&>
  auto listen(endpoint const& ep, int backlog, Configure&& configure) -> result<void> {
    auto open_r = ok();
    if (!is_open()) {
      open_r = handle_.impl().open(ep.family(), Protocol::type(), Protocol::protocol());
    }
    return open_r
      .and_then([&] {
        std::invoke(std::forward<Configure>(configure), *this);
        return handle_.impl().bind(ep.data(), ep.size());
      })
      .and_then([&] { return handle_.impl().listen(backlog); });
  }

  auto local_endpoint() const -> result<endpoint> {
    return ::iocoro::detail::socket::get_local_endpoint<endpoint>(handle_.native_handle());
  }

  /// Accept and return a connected `socket`.
  ///
  /// Notes:
  /// - The returned socket is bound to the same io_context as this acceptor.
  /// - The accepted native fd is adopted atomically; no fd leaks occur on failure.
  auto async_accept() -> awaitable<result<socket>> {
    auto r = co_await async_accept_fd();
    if (!r) {
      co_return unexpected(r.error());
    }
    socket s{handle_.get_executor()};
    auto ar = s.assign(*r);
    if (!ar) {
      co_return unexpected(ar.error());
    }
    co_return s;
  }

  auto get_executor() const noexcept -> any_io_executor { return handle_.get_executor(); }

  auto native_handle() const noexcept -> int { return handle_.native_handle(); }

  auto close() noexcept -> result<void> {
    return handle_.close();
  }
  auto is_open() const noexcept -> bool { return handle_.is_open(); }

  void cancel() noexcept { handle_.cancel(); }
  void cancel_read() noexcept { handle_.cancel_read(); }

  template <class Option>
  auto set_option(Option const& opt) -> result<void> {
    return handle_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> result<void> {
    return handle_.get_option(opt);
  }

 private:
  /// Accept and return the connected native fd (low-level building block).
  auto async_accept_fd() -> awaitable<result<int>> {
    co_return co_await handle_.impl().async_accept();
  }

  handle_type handle_;
};

}  // namespace iocoro::net
