#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/cancellation_token.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_async.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io_executor.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
  /// Wait until expiry (or cancellation) as an awaitable, observing a cancellation token.
  auto async_wait(use_awaitable_t, cancellation_token tok = {}) -> awaitable<std::error_code> {
    co_return co_await detail::operation_awaiter<timer_wait_operation>{this, std::move(tok)};
  }

  /// Cancel the pending timer operation.
  void cancel() noexcept {
    // Increment cancellation sequence to allow pending arming to observe cancellation.
    cancel_seq_.fetch_add(1, std::memory_order_acq_rel);

    detail::io_context_impl::timer_event_handle h{};
    {
      std::scoped_lock lk{mtx_};
      h = std::exchange(handle_, detail::io_context_impl::timer_event_handle::invalid_handle());
    }

    if (h) {
      h.cancel();
    }
  }

  time_point expiry() { return expiry_; }

  auto cancel_seq() const noexcept -> std::uint64_t {
    return cancel_seq_.load(std::memory_order_acquire);
  }

  void set_write_handle(detail::io_context_impl::timer_event_handle h) noexcept {
    std::scoped_lock lk{mtx_};
    handle_ = h;
  }

 private:
  class timer_wait_operation final : public detail::async_operation {
   public:
    timer_wait_operation(std::shared_ptr<detail::operation_wait_state> st,
                         steady_timer* timer,
                         cancellation_token tok)
        : async_operation(std::move(st)), timer_(timer), tok_(std::move(tok)) {
      // Bind cancellation to the operation lifetime.
      // This avoids TOCTOU windows between cancellation and timer arming.
      reg_ = tok_.register_callback([this] { this->timer_->cancel(); });
      start_seq_ = timer_->cancel_seq();
    }

   private:
    void do_start(std::unique_ptr<operation_base> self) override {
      // If cancellation already requested, abort without arming.
      if (tok_.stop_requested()) {
        self->on_abort(error::operation_aborted);
        return;
      }

      auto handle = timer_->ctx_impl_->schedule_timer(timer_->expiry(), std::move(self));
      timer_->set_write_handle(handle);

      // If cancellation raced with arming, ensure the newly-armed timer is cancelled.
      if (timer_->cancel_seq() != start_seq_) {
        timer_->cancel();
      }
    }

    steady_timer* timer_ = nullptr;
    cancellation_token tok_{};
    cancellation_registration reg_{};
    std::uint64_t start_seq_{0};
  };

  detail::io_context_impl* ctx_impl_;
  time_point expiry_{clock::now()};
  mutable std::mutex mtx_{};
  std::atomic<std::uint64_t> cancel_seq_{0};
  detail::io_context_impl::timer_event_handle handle_{};
};

}  // namespace iocoro
