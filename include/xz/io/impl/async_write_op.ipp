#include <xz/io/detail/async_write_op.hpp>
#include <xz/io/detail/tcp_socket_impl.hpp>
#include <xz/io/detail/operation_base.hpp>

namespace xz::io {

async_write_some_op::async_write_some_op(std::weak_ptr<detail::tcp_socket_impl> socket_impl,
                                          std::span<char const> buf,
                                          std::chrono::milliseconds timeout)
    : async_io_operation<std::size_t>(std::move(socket_impl), timeout), buffer_(buf) {}

void async_write_some_op::start_operation() {
  auto socket_impl = get_socket_impl();
  if (!socket_impl) {
    complete(error::operation_aborted);
    return;
  }

  auto result = socket_impl->write_some(buffer_);
  if (result) {
    complete({}, *result);
    return;
  }

  auto ec = result.error();
  if (ec != std::errc::operation_would_block) {
    complete(ec);
    return;
  }

  setup_timeout();

  struct write_operation : operation_base {
    std::weak_ptr<detail::tcp_socket_impl> socket_impl;
    std::span<char const> buffer;
    async_write_some_op* op;

    write_operation(std::weak_ptr<detail::tcp_socket_impl> s, std::span<char const> buf,
                    async_write_some_op* o)
        : socket_impl(std::move(s)), buffer(buf), op(o) {}

    void execute() override {
      auto socket_impl_ptr = socket_impl.lock();
      if (!socket_impl_ptr) {
        op->complete(error::operation_aborted);
        return;
      }

      auto result = socket_impl_ptr->write_some(buffer);

      if (!result && result.error() == std::errc::operation_would_block) {
        socket_impl_ptr->get_executor().register_fd_write(socket_impl_ptr->native_handle(),
                                                          std::make_unique<write_operation>(socket_impl, buffer, op));
        return;
      }

      op->cleanup_timer();

      if (result) {
        op->complete({}, *result);
      } else {
        op->complete(result.error());
      }
    }

    void abort(std::error_code ec) override {
      op->cleanup_timer();
      op->complete(ec);
    }
  };

  socket_impl->get_executor().register_fd_write(socket_impl->native_handle(),
                                                std::make_unique<write_operation>(socket_impl_, buffer_, this));
}

}  // namespace xz::io
