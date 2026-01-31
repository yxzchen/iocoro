#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/socket_handle_base.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/shutdown.hpp>
#include <iocoro/error.hpp>

#include <iocoro/detail/socket/stream_socket_impl.hpp>
#include <iocoro/detail/socket_endpoint_utils.hpp>

#include <cstddef>
#include <span>
#include <system_error>

namespace iocoro::net {

/// Protocol-typed stream socket facade (network semantic layer).
///
/// Layering / responsibilities (important):
/// - `iocoro::detail::socket_handle_base<Impl>` is a small protocol-agnostic PImpl wrapper
///   (fd lifecycle, cancel/close, socket options, native_handle).
/// - `iocoro::net::basic_stream_socket<Protocol>` is the protocol-typed *network facade*
///   providing connect/read/write/endpoint/shutdown semantics.
/// - The underlying implementation is `iocoro::detail::socket::stream_socket_impl`
///   (protocol-agnostic stream IO).
/// - Protocol semantics (endpoint conversion, socket type/protocol) are handled here in the facade.
///
/// Construction:
/// - No default constructor: a socket must be bound to an IO executor (or io_context) up-front.
/// - Protocol is fixed by the template parameter; there is no "rebind protocol" behavior.
template <class Protocol>
class basic_stream_socket
    : public ::iocoro::detail::socket_handle_base<::iocoro::detail::socket::stream_socket_impl> {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;
  using impl_type = ::iocoro::detail::socket::stream_socket_impl;
  using base_type = ::iocoro::detail::socket_handle_base<impl_type>;

  basic_stream_socket() = delete;

  explicit basic_stream_socket(any_io_executor ex) : base_type(ex) {}
  explicit basic_stream_socket(io_context& ctx) : base_type(ctx) {}

  basic_stream_socket(basic_stream_socket const&) = delete;
  auto operator=(basic_stream_socket const&) -> basic_stream_socket& = delete;

  basic_stream_socket(basic_stream_socket&&) = default;
  auto operator=(basic_stream_socket&&) -> basic_stream_socket& = default;

  auto async_connect(endpoint const& ep) -> awaitable<std::error_code> {
    // Lazy-open based on endpoint family; protocol specifics come from Protocol tag.
    if (!this->impl_->is_open()) {
      auto ec = this->impl_->open(ep.family(), Protocol::type(), Protocol::protocol());
      if (ec) {
        co_return ec;
      }
    }
    co_return co_await this->impl_->async_connect(ep.data(), ep.size());
  }

  auto async_read_some(std::span<std::byte> buffer)
    -> awaitable<expected<std::size_t, std::error_code>> {
    co_return co_await this->impl_->async_read_some(buffer);
  }

  auto async_write_some(std::span<std::byte const> buffer)
    -> awaitable<expected<std::size_t, std::error_code>> {
    co_return co_await this->impl_->async_write_some(buffer);
  }

  auto local_endpoint() const -> expected<endpoint, std::error_code> {
    return ::iocoro::detail::socket::get_local_endpoint<endpoint>(this->impl_->native_handle());
  }

  auto remote_endpoint() const -> expected<endpoint, std::error_code> {
    auto const fd = this->impl_->native_handle();
    if (fd < 0) {
      return unexpected(error::not_open);
    }
    if (!this->impl_->is_connected()) {
      return unexpected(error::not_connected);
    }
    return ::iocoro::detail::socket::get_remote_endpoint<endpoint>(fd);
  }

  auto shutdown(shutdown_type what) -> std::error_code { return this->impl_->shutdown(what); }

  auto is_connected() const noexcept -> bool { return this->impl_->is_connected(); }

  using base_type::native_handle;

  using base_type::close;
  using base_type::is_open;

  using base_type::cancel;
  using base_type::cancel_read;
  using base_type::cancel_write;

  using base_type::get_option;
  using base_type::set_option;

 private:
  template <class P>
  friend class basic_acceptor;

  // Internal hook for acceptors: adopt a connected fd from accept().
  auto assign(int fd) -> std::error_code { return this->impl_->assign(fd); }
};

}  // namespace iocoro::net
