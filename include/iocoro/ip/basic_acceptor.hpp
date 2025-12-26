#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/basic_io_handle.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>

#include <iocoro/detail/ip/basic_acceptor_impl.hpp>
#include <iocoro/ip/basic_stream_socket.hpp>

#include <system_error>

namespace iocoro::ip {

/// Protocol-typed acceptor facade (network semantic layer).
///
/// This is a networking facade layered on top of:
/// - `iocoro::detail::basic_io_handle<Impl>`: a small, reusable PImpl wrapper that provides
///   fd lifecycle and common cancellation/option APIs.
/// - `iocoro::detail::ip::basic_acceptor_impl<Protocol>`: protocol-injected implementation.
///
/// Important:
/// - This type is protocol-typed (via `Protocol` template parameter).
/// - `async_accept()` returns a connected `basic_stream_socket<Protocol>` and adopts the
///   accepted native fd internally.
template <class Protocol>
class basic_acceptor
    : public ::iocoro::detail::basic_io_handle<detail::ip::basic_acceptor_impl<Protocol>> {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;
  using socket = basic_stream_socket<Protocol>;
  using impl_type = detail::ip::basic_acceptor_impl<Protocol>;
  using base_type = ::iocoro::detail::basic_io_handle<impl_type>;

  basic_acceptor() = delete;

  explicit basic_acceptor(executor ex) : base_type(ex) {}
  explicit basic_acceptor(io_context& ctx) : base_type(ctx) {}

  basic_acceptor(basic_acceptor const&) = delete;
  auto operator=(basic_acceptor const&) -> basic_acceptor& = delete;

  basic_acceptor(basic_acceptor&&) = default;
  auto operator=(basic_acceptor&&) -> basic_acceptor& = default;

  auto open(int family) -> std::error_code { return this->impl_->open(family); }

  auto bind(endpoint const& ep) -> std::error_code { return this->impl_->bind(ep); }

  auto listen(int backlog = 0) -> std::error_code { return this->impl_->listen(backlog); }

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
  /// - The returned socket is bound to the same executor as this acceptor.
  /// - The accepted native fd is adopted atomically; no fd leaks occur on failure.
  auto async_accept() -> awaitable<expected<socket, std::error_code>> {
    auto r = co_await async_accept_fd();
    if (!r) {
      co_return unexpected(r.error());
    }
    socket s{this->get_executor()};
    if (auto ec = s.assign(*r)) {
      co_return unexpected(ec);
    }
    co_return s;
  }

  using base_type::get_executor;
  using base_type::native_handle;

  using base_type::close;
  using base_type::is_open;

  using base_type::cancel;
  using base_type::cancel_read;

  using base_type::get_option;
  using base_type::set_option;
};

}  // namespace iocoro::ip


