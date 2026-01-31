#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_awaiter.hpp>
#include <iocoro/error.hpp>
#include <chrono>
#include <cstddef>
#include <system_error>

namespace iocoro {

/// A single-use timer for awaiting a time point.
///
/// Model:
/// - Each async_wait() creates a new timer operation.
/// - cancel() cancels the current pending timer.
/// - Updating expiry affects subsequent waits.
class steady_timer {
 public:
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  explicit steady_timer(any_io_executor ex) noexcept
      : ctx_impl_(ex.io_context_ptr()), expiry_(clock::now()) {}
  steady_timer(any_io_executor ex, time_point at) noexcept
      : ctx_impl_(ex.io_context_ptr()), expiry_(at) {}
  steady_timer(any_io_executor ex, duration after) noexcept
      : ctx_impl_(ex.io_context_ptr()), expiry_(clock::now() + after) {}

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

  /// Wait until expiry as an awaitable.
  ///
  /// Returns:
  /// - `std::error_code{}` on successful timer expiry.
  auto async_wait(use_awaitable_t) -> awaitable<std::error_code> {
    if (!ctx_impl_) {
      co_return error::invalid_argument;
    }
    auto* timer = this;
    co_return co_await detail::operation_awaiter{
      [timer](detail::reactor_op_ptr rop) {
        auto h = timer->ctx_impl_->add_timer(timer->expiry(), std::move(rop));
        timer->set_timer_handle(h);
        return h;
      }};
  }

  /// Cancel the pending timer operation.
  void cancel() noexcept {
    if (!ctx_impl_) {
      return;
    }
    detail::io_context_impl::event_handle h{};
    {
      std::scoped_lock lk{mtx_};
      h = std::exchange(handle_, detail::io_context_impl::event_handle::invalid_handle());
    }

    if (h) {
      h.cancel();
    }
  }

  time_point expiry() { return expiry_; }

  void set_timer_handle(detail::io_context_impl::event_handle h) noexcept {
    std::scoped_lock lk{mtx_};
    handle_ = h;
  }

 private:
  detail::io_context_impl* ctx_impl_;
  time_point expiry_{clock::now()};
  mutable std::mutex mtx_{};
  detail::io_context_impl::event_handle handle_{};
};

}  // namespace iocoro
