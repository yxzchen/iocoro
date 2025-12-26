#include <iocoro/detail/ip/tcp/acceptor_impl.hpp>
#include <iocoro/ip/tcp/acceptor.hpp>

namespace iocoro::ip::tcp {

inline acceptor::acceptor(executor ex) : base_type(ex) {}

inline acceptor::acceptor(io_context& ctx) : base_type(ctx) {}

inline auto acceptor::open(int family) -> std::error_code { return impl_->open(family); }

inline auto acceptor::bind(endpoint const& ep) -> std::error_code { return impl_->bind(ep); }

inline auto acceptor::listen(int backlog) -> std::error_code { return impl_->listen(backlog); }

inline auto acceptor::local_endpoint() const -> expected<endpoint, std::error_code> {
  return impl_->local_endpoint();
}

inline auto acceptor::async_accept() -> awaitable<expected<socket, std::error_code>> {
  auto r = co_await impl_->async_accept();
  if (!r) {
    co_return unexpected(r.error());
  }

  socket s{get_executor()};
  if (auto ec = s.assign(*r)) {
    co_return unexpected(ec);
  }
  co_return s;
}

}  // namespace iocoro::ip::tcp
