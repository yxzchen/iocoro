#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_async.hpp>
#include <iocoro/io_executor.hpp>

#include <chrono>
#include <cstddef>
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

  explicit steady_timer(io_executor ex) noexcept : ctx_impl_(ex.impl_), expiry_(clock::now()) {}
  steady_timer(io_executor ex, time_point at) noexcept : ctx_impl_(ex.impl_), expiry_(at) {}
  steady_timer(io_executor ex, duration after) noexcept
      : ctx_impl_(ex.impl_), expiry_(clock::now() + after) {}

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
  /// - `error::operation_aborted` if the timer was cancelled.
  auto async_wait(use_awaitable_t) -> awaitable<std::error_code> {
    co_return co_await detail::operation_awaiter<timer_wait_operation>{this};
  }

  /// Cancel the pending timer operation.
  void cancel() noexcept {
    if (handle_) {
      handle_.cancel();
      handle_ = detail::io_context_impl::timer_event_handle::invalid_handle();
    }
  }

  time_point expiry() { return expiry_; }

  void set_write_handle(detail::io_context_impl::timer_event_handle h) noexcept { handle_ = h; }

 private:
  class timer_wait_operation final : public detail::async_operation {
   public:
    timer_wait_operation(std::shared_ptr<detail::operation_wait_state> st, steady_timer* timer)
        : async_operation(std::move(st)), timer_(timer) {}

   private:
    void do_start(std::unique_ptr<operation_base> self) override {
      auto handle = timer_->ctx_impl_->schedule_timer(timer_->expiry(), std::move(self));
      timer_->set_write_handle(handle);
    }

    steady_timer* timer_ = nullptr;
  };

  detail::io_context_impl* ctx_impl_;
  time_point expiry_{clock::now()};
  detail::io_context_impl::timer_event_handle handle_{};
};

}  // namespace iocoro
