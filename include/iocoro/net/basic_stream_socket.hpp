#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/detail/socket_handle_base.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/result.hpp>
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
class basic_stream_socket {
 public:
  using protocol_type = Protocol;
  using endpoint = typename Protocol::endpoint;
  using impl_type = ::iocoro::detail::socket::stream_socket_impl;
  using handle_type = ::iocoro::detail::socket_handle_base<impl_type>;

  basic_stream_socket() = delete;

  explicit basic_stream_socket(any_io_executor ex) : handle_(std::move(ex)) {}
  explicit basic_stream_socket(io_context& ctx) : handle_(ctx) {}

  basic_stream_socket(basic_stream_socket const&) = delete;
  auto operator=(basic_stream_socket const&) -> basic_stream_socket& = delete;

  basic_stream_socket(basic_stream_socket&&) = default;
  auto operator=(basic_stream_socket&&) -> basic_stream_socket& = default;

  auto async_connect(endpoint const& ep) -> awaitable<result<void>> {
    // Lazy-open based on endpoint family; protocol specifics come from Protocol tag.
    if (!handle_.impl().is_open()) {
      auto ec = handle_.impl().open(ep.family(), Protocol::type(), Protocol::protocol());
      if (ec) {
        co_return fail(ec);
      }
    }
    auto ec = co_await handle_.impl().async_connect(ep.data(), ep.size());
    if (ec) {
      co_return fail(ec);
    }
    co_return ok();
  }

  auto async_read_some(std::span<std::byte> buffer)
    -> awaitable<result<std::size_t>> {
    co_return co_await handle_.impl().async_read_some(buffer);
  }

  auto async_write_some(std::span<std::byte const> buffer)
    -> awaitable<result<std::size_t>> {
    co_return co_await handle_.impl().async_write_some(buffer);
  }

  auto local_endpoint() const -> result<endpoint> {
    return ::iocoro::detail::socket::get_local_endpoint<endpoint>(handle_.native_handle());
  }

  auto remote_endpoint() const -> result<endpoint> {
    auto const fd = handle_.native_handle();
    if (fd < 0) {
      return unexpected(error::not_open);
    }
    if (!handle_.impl().is_connected()) {
      return unexpected(error::not_connected);
    }
    return ::iocoro::detail::socket::get_remote_endpoint<endpoint>(fd);
  }

  auto shutdown(shutdown_type what) -> std::error_code { return handle_.impl().shutdown(what); }

  auto is_connected() const noexcept -> bool { return handle_.impl().is_connected(); }

  auto get_executor() const noexcept -> any_io_executor { return handle_.get_executor(); }

  auto native_handle() const noexcept -> int { return handle_.native_handle(); }

  auto close() noexcept -> std::error_code { return handle_.close(); }
  auto is_open() const noexcept -> bool { return handle_.is_open(); }

  void cancel() noexcept { handle_.cancel(); }
  void cancel_read() noexcept { handle_.cancel_read(); }
  void cancel_write() noexcept { handle_.cancel_write(); }

  template <class Option>
  auto set_option(Option const& opt) -> std::error_code {
    return handle_.set_option(opt);
  }

  template <class Option>
  auto get_option(Option& opt) -> std::error_code {
    return handle_.get_option(opt);
  }

 private:
  template <class P>
  friend class basic_acceptor;

  // Internal hook for acceptors: adopt a connected fd from accept().
  auto assign(int fd) -> std::error_code { return handle_.impl().assign(fd); }

  handle_type handle_;
};

}  // namespace iocoro::net
