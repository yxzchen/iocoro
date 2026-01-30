#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/async_op.hpp>
#include <iocoro/detail/operation_awaiter.hpp>
#include <stop_token>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <system_error>

namespace iocoro {

/// A single-use timer for awaiting a time point.
///
/// Model:
/// - Each async_wait() creates a new timer operation.
/// - cancel() cancels the current pending timer.
/// - Setting a new expiry (expires_at/after) cancels the previous timer.
class steady_timer {
 public:
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  explicit steady_timer(any_io_executor ex) noexcept
      : ctx_impl_(detail::get_reactor_access(ex.as_any_executor()).impl),
        expiry_(clock::now()) {
    IOCORO_ENSURE(ctx_impl_, "steady_timer: requires IO executor");
  }
  steady_timer(any_io_executor ex, time_point at) noexcept
      : ctx_impl_(detail::get_reactor_access(ex.as_any_executor()).impl), expiry_(at) {
    IOCORO_ENSURE(ctx_impl_, "steady_timer: requires IO executor");
  }
  steady_timer(any_io_executor ex, duration after) noexcept
      : ctx_impl_(detail::get_reactor_access(ex.as_any_executor()).impl),
        expiry_(clock::now() + after) {
    IOCORO_ENSURE(ctx_impl_, "steady_timer: requires IO executor");
  }

  steady_timer(steady_timer const&) = delete;
  auto operator=(steady_timer const&) -> steady_timer& = delete;
  steady_timer(steady_timer&&) = delete;
  auto operator=(steady_timer&&) -> steady_timer& = delete;

  ~steady_timer() { cancel(); }

  auto expiry() const noexcept -> time_point { return expiry_; }

  /// Set the timer expiry time.
  void expires_at(time_point at) noexcept {
    expiry_ = at;
    cancel();
  }

  /// Set the timer expiry time relative to now.
  void expires_after(duration d) noexcept {
    expiry_ = clock::now() + d;
    cancel();
  }

  /// Wait until expiry (or cancellation) as an awaitable.
  ///
  /// Returns:
  /// - `std::error_code{}` on successful timer expiry.
  /// - `error::operation_aborted` if cancelled via current coroutine cancellation context.
  auto async_wait(use_awaitable_t) -> awaitable<std::error_code> {
    co_return co_await detail::operation_awaiter{
      [this](std::shared_ptr<detail::operation_wait_state> st) {
        return make_wait_op(std::move(st));
      }};
  }

  /// Cancel the pending timer operation.
  void cancel() noexcept {
    detail::io_context_impl::timer_event_handle h{};
    {
      std::scoped_lock lk{mtx_};
      h = std::exchange(handle_, detail::io_context_impl::timer_event_handle::invalid_handle());
    }

    if (h) {
      ctx_impl_->cancel_timer(h);
    }
  }

  time_point expiry() { return expiry_; }

  void set_timer_handle(detail::io_context_impl::timer_event_handle h) noexcept {
    std::scoped_lock lk{mtx_};
    handle_ = h;
  }

 private:
  auto make_wait_op(std::shared_ptr<detail::operation_wait_state> st) -> detail::async_op {
    auto access = detail::reactor_access{ctx_impl_};
    auto* timer = this;
    return detail::async_op{
      std::move(st),
      access,
      [timer](detail::reactor_access const& access, detail::reactor_op_ptr rop) {
        auto h = access.get().add_timer(timer->expiry(), std::move(rop));
        timer->set_timer_handle(h);
        return detail::io_context_impl::event_handle::make(std::move(h));
      }};
  }

  detail::io_context_impl* ctx_impl_;
  time_point expiry_{clock::now()};
  mutable std::mutex mtx_{};
  detail::io_context_impl::timer_event_handle handle_{};
};

}  // namespace iocoro
