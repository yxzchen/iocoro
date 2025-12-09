
#include <xz/io/detail/io_context_impl.hpp>

#ifdef IOXZ_HAS_URING
#include <xz/io/impl/backends/uring.ipp>
#else
#include <xz/io/impl/backends/epoll.ipp>
#endif

namespace xz::io {

io_context::io_context() : impl_(std::make_unique<detail::io_context_impl>()) {}

io_context::~io_context() = default;

io_context::io_context(io_context&&) noexcept = default;

auto io_context::operator=(io_context&&) noexcept -> io_context& = default;

auto io_context::run() -> std::size_t { return impl_->run(); }

auto io_context::run_one() -> std::size_t { return impl_->run_one(); }

auto io_context::run_for(std::chrono::milliseconds timeout) -> std::size_t {
  return impl_->run_for(timeout);
}

void io_context::stop() { impl_->stop(); }

void io_context::restart() { impl_->restart(); }

auto io_context::stopped() const noexcept -> bool { return impl_->stopped(); }

void io_context::post(std::function<void()> f) { impl_->post(std::move(f)); }

void io_context::dispatch(std::function<void()> f) { impl_->dispatch(std::move(f)); }

auto io_context::native_handle() const noexcept -> int { return impl_->native_handle(); }

void io_context::register_fd_read(int fd, std::unique_ptr<operation_base> op) {
  impl_->register_fd_read(fd, std::move(op));
}

void io_context::register_fd_write(int fd, std::unique_ptr<operation_base> op) {
  impl_->register_fd_write(fd, std::move(op));
}

void io_context::register_fd_readwrite(int fd, std::unique_ptr<operation_base> read_op,
                                       std::unique_ptr<operation_base> write_op) {
  impl_->register_fd_readwrite(fd, std::move(read_op), std::move(write_op));
}

void io_context::deregister_fd(int fd) { impl_->deregister_fd(fd); }

auto io_context::schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback)
    -> detail::timer_handle {
  return impl_->schedule_timer(timeout, std::move(callback));
}

void io_context::cancel_timer(detail::timer_handle handle) { impl_->cancel_timer(std::move(handle)); }

}  // namespace xz::io
