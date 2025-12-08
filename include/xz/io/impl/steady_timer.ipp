#pragma once

#include <xz/io/detail/steady_timer_impl.hpp>
#include <xz/io/error.hpp>

namespace xz::io::detail {

auto steady_timer_impl::wait(steady_timer::duration d) -> std::error_code {
  cancel();

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d);
  if (ms.count() < 0) {
    return make_error_code(error::invalid_argument);
  }

  return {};  // Timer will be set up by the async operation
}

}  // namespace xz::io::detail

// steady_timer implementation

namespace xz::io {

steady_timer::steady_timer(io_context& ctx)
    : impl_(std::make_unique<detail::steady_timer_impl>(ctx)) {}

steady_timer::~steady_timer() = default;

steady_timer::steady_timer(steady_timer&&) noexcept = default;

auto steady_timer::operator=(steady_timer&&) noexcept -> steady_timer& = default;

auto steady_timer::get_executor() noexcept -> io_context& { return impl_->get_executor(); }

void steady_timer::cancel() { impl_->cancel(); }

// Async operation implementation

steady_timer::async_wait_op::async_wait_op(steady_timer& t, duration d)
    : timer_(t), duration_(d) {}

void steady_timer::async_wait_op::start_operation() {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration_);

  timer_.impl_->timer_id_ = timer_.get_executor().schedule_timer(ms, [this]() {
    timer_.impl_->timer_id_ = 0;
    complete({});
  });
}

}  // namespace xz::io
