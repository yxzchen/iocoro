#include <iocoro/ip/tcp_socket.hpp>

#include <iocoro/error.hpp>

namespace iocoro::ip {

inline tcp_socket::tcp_socket(::iocoro::executor ex) : base_type(ex) {}

inline tcp_socket::tcp_socket(::iocoro::io_context& ctx) : base_type(ctx) {}

inline auto tcp_socket::async_connect(::iocoro::use_awaitable_t, endpoint const& ep)
  -> ::iocoro::awaitable<std::error_code> {
  if (!impl_) {
    co_return ::iocoro::make_error_code(::iocoro::error::not_open);
  }
  co_return co_await impl_->async_connect(::iocoro::use_awaitable, ep);
}

inline auto tcp_socket::async_read_some(::iocoro::use_awaitable_t, void* data, std::size_t size)
  -> ::iocoro::awaitable<::iocoro::expected<std::size_t, std::error_code>> {
  if (!impl_) {
    co_return ::iocoro::unexpected<std::error_code>(
      ::iocoro::make_error_code(::iocoro::error::not_open));
  }
  co_return co_await impl_->async_read_some(::iocoro::use_awaitable, data, size);
}

inline auto tcp_socket::async_write_some(::iocoro::use_awaitable_t, void const* data,
                                         std::size_t size)
  -> ::iocoro::awaitable<::iocoro::expected<std::size_t, std::error_code>> {
  if (!impl_) {
    co_return ::iocoro::unexpected<std::error_code>(
      ::iocoro::make_error_code(::iocoro::error::not_open));
  }
  co_return co_await impl_->async_write_some(::iocoro::use_awaitable, data, size);
}

}  // namespace iocoro::ip
