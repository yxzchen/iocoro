#include <xz/io/detail/async_connect_op.hpp>
#include <xz/io/detail/tcp_socket_impl.hpp>
#include <xz/io/detail/operation_base.hpp>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>

namespace xz::io {

async_connect_op::async_connect_op(std::weak_ptr<detail::tcp_socket_impl> socket_impl,
                                    ip::tcp_endpoint ep,
                                    std::chrono::milliseconds timeout)
    : async_io_operation<void>(std::move(socket_impl), timeout), endpoint_(ep) {}

void async_connect_op::start_operation() {
  auto socket_impl = get_socket_impl();
  if (!socket_impl) {
    complete(error::operation_aborted);
    return;
  }

  auto ec = socket_impl->connect(endpoint_);
  if (ec) {
    if (ec != std::errc::operation_in_progress) {
      complete(ec);
      return;
    }
  }

  setup_timeout();

  struct connect_operation : operation_base {
    std::weak_ptr<detail::tcp_socket_impl> socket_impl;
    async_connect_op* op;

    connect_operation(std::weak_ptr<detail::tcp_socket_impl> s, async_connect_op* o)
        : socket_impl(std::move(s)), op(o) {}

    void execute() override {
      auto socket_impl_ptr = socket_impl.lock();
      if (!socket_impl_ptr) {
        op->complete(error::operation_aborted);
        return;
      }

      int error = 0;
      socklen_t len = sizeof(error);
      ::getsockopt(socket_impl_ptr->native_handle(), SOL_SOCKET, SO_ERROR, &error, &len);

      socket_impl_ptr->get_executor().deregister_fd(socket_impl_ptr->native_handle());
      op->cleanup_timer();

      if (error) {
        op->complete(std::error_code(error, std::generic_category()));
      } else {
        op->complete({});
      }
    }

    void abort(std::error_code ec) override {
      op->cleanup_timer();
      op->complete(ec);
    }
  };

  socket_impl->get_executor().register_fd_write(socket_impl->native_handle(),
                                                std::make_unique<connect_operation>(socket_impl_, this));
}

}  // namespace xz::io
