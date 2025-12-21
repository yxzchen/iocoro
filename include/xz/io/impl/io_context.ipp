#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/executor.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/timer_handle.hpp>

#include <utility>

namespace xz::io {

io_context::io_context() : impl_(std::make_unique<detail::io_context_impl>()) {}

io_context::~io_context() = default;

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

auto io_context::schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback)
  -> timer_handle {
  auto entry = impl_->schedule_timer(timeout, std::move(callback));
  return timer_handle(entry);
}

auto io_context::get_executor() noexcept -> executor { return executor{*impl_}; }

}  // namespace xz::io
