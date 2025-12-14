#include <xz/io/detail/tcp_socket_impl.hpp>

namespace xz::io {

template <typename Result>
void async_io_operation<Result>::setup_timeout() {
  if (timeout_.count() > 0) {
    auto socket_impl = get_socket_impl();
    if (!socket_impl) return;

    timer_handle_ = socket_impl->get_executor().schedule_timer(timeout_, [this, weak_socket_impl = socket_impl_]() {
      auto socket_impl = weak_socket_impl.lock();
      if (!socket_impl) {
        this->complete(error::operation_aborted);
        return;
      }

      socket_impl->get_executor().deregister_fd(socket_impl->native_handle());
      timer_handle_.reset();
      if constexpr (std::is_void_v<Result>) {
        this->complete(error::timeout);
      } else {
        this->complete(error::timeout, Result{});
      }
    });
  }
}

template <typename Result>
void async_io_operation<Result>::cleanup_timer() {
  if (timer_handle_) {
    auto socket_impl = get_socket_impl();
    if (socket_impl) {
      socket_impl->get_executor().cancel_timer(timer_handle_);
    }
    timer_handle_.reset();
  }
}

}  // namespace xz::io
