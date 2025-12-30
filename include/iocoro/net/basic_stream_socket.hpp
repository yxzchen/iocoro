#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/basic_io_handle.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/shutdown.hpp>

#include <iocoro/detail/net/basic_stream_socket_impl.hpp>

#include <cstddef>
#include <span>
#include <system_error>

namespace iocoro::net {

/// Protocol-typed stream socket facade (network semantic layer).
///
/// Layering / responsibilities (important):
/// - `iocoro::detail::basic_io_handle<Impl>` is a small protocol-agnostic PImpl wrapper
///   (fd lifecycle, cancel/close, socket options, native_handle).
/// - `iocoro::net::basic_stream_socket<Protocol>` is the protocol-typed *network facade*
///   providing connect/read/write/endpoint/shutdown semantics.
/// - The protocol-injected implementation is
/// `iocoro::detail::net::basic_stream_socket_impl<Protocol>`,
///   built on top of `iocoro::detail::socket::stream_socket_impl` (protocol-agnostic stream IO).
///
/// Construction:
/// - No default constructor: a socket must be bound to an io_executor (or io_context) up-front.
/// - Protocol is fixed by the template parameter; there is no "rebind protocol" behavior.
template <class Protocol>
class basic_stream_socket : public ::iocoro::detail::basic_io_handle<
                              ::iocoro::detail::net::basic_stream_socket_impl<Protocol>> {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;
  using impl_type = ::iocoro::detail::net::basic_stream_socket_impl<Protocol>;
  using base_type = ::iocoro::detail::basic_io_handle<impl_type>;

  basic_stream_socket() = delete;

  explicit basic_stream_socket(io_executor ex) : base_type(ex) {}
  explicit basic_stream_socket(io_context& ctx) : base_type(ctx) {}

  basic_stream_socket(basic_stream_socket const&) = delete;
  auto operator=(basic_stream_socket const&) -> basic_stream_socket& = delete;

  basic_stream_socket(basic_stream_socket&&) = default;
  auto operator=(basic_stream_socket&&) -> basic_stream_socket& = default;

  auto async_connect(endpoint const& ep) -> awaitable<std::error_code> {
    co_return co_await this->impl_->async_connect(ep);
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
    return this->impl_->local_endpoint();
  }
  auto remote_endpoint() const -> expected<endpoint, std::error_code> {
    return this->impl_->remote_endpoint();
  }

  auto shutdown(shutdown_type what) -> std::error_code { return this->impl_->shutdown(what); }

  auto is_connected() const noexcept -> bool { return this->impl_->is_connected(); }

  using base_type::get_executor;
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
