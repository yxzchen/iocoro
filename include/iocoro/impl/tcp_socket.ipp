#include <iocoro/ip/tcp_socket.hpp>

#include <iocoro/error.hpp>

namespace iocoro::ip {

inline tcp_socket::tcp_socket(executor ex) : base_type(ex) {}

inline tcp_socket::tcp_socket(io_context& ctx) : base_type(ctx) {}

inline auto tcp_socket::async_connect(use_awaitable_t, endpoint const& ep)
  -> awaitable<std::error_code> {
  if (!impl_) {
    co_return error::not_open;
  }
  co_return co_await impl_->async_connect(use_awaitable, ep);
}

inline auto tcp_socket::async_read_some(use_awaitable_t, void* data, std::size_t size)
  -> awaitable<expected<std::size_t, std::error_code>> {
  if (!impl_) {
    co_return unexpected<std::error_code>(error::not_open);
  }
  co_return co_await impl_->async_read_some(use_awaitable, data, size);
}

inline auto tcp_socket::async_write_some(use_awaitable_t, void const* data, std::size_t size)
  -> awaitable<expected<std::size_t, std::error_code>> {
  if (!impl_) {
    co_return unexpected<std::error_code>(error::not_open);
  }
  co_return co_await impl_->async_write_some(use_awaitable, data, size);
}

}  // namespace iocoro::ip
