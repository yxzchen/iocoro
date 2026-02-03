#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_awaiter.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <iocoro/this_coro.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <system_error>
#include <utility>

namespace iocoro {

/// A single-use timer for awaiting a time point.
///
/// Semantics:
/// - Each `async_wait()` creates a new timer registration in the underlying `io_context`.
/// - `cancel()` cancels the current pending registration (if any).
/// - Updating expiry affects subsequent waits; it does not mutate an already-started wait.
class steady_timer {
 public:
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  explicit steady_timer(any_io_executor ex, time_point at) noexcept
      : ex_(std::move(ex)), ctx_impl_(ex_.io_context_ptr()), expiry_(at) {
    IOCORO_ENSURE(ex_, "steady_timer: requires a non-empty IO executor");
    IOCORO_ENSURE(ctx_impl_ != nullptr, "steady_timer: missing io_context_impl");
  }

  explicit steady_timer(any_io_executor ex) noexcept : steady_timer(std::move(ex), clock::now()) {}

  explicit steady_timer(any_io_executor ex, duration after) noexcept
      : steady_timer(std::move(ex), clock::now() + after) {}

  steady_timer(steady_timer const&) = delete;
  auto operator=(steady_timer const&) -> steady_timer& = delete;
  steady_timer(steady_timer&&) = delete;
  auto operator=(steady_timer&&) -> steady_timer& = delete;

  ~steady_timer() noexcept { cancel(); }

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
  /// - `ok()` on successful timer expiry.
  auto async_wait(use_awaitable_t) -> awaitable<result<void>> {
    auto* timer = this;
    // IMPORTANT: timer registration mutates reactor-owned state; do it on the reactor thread.
    // NOTE: If we are already on that thread, avoid an extra hop (keeps run_one()-style
    // loops deterministic w.r.t. register/cancel ordering).
    auto orig_ex = co_await this_coro::executor;
    IOCORO_ENSURE(orig_ex, "steady_timer::async_wait: requires a bound executor");
    co_await this_coro::switch_to(ex_);
    auto r = co_await detail::operation_awaiter{
      [timer](detail::reactor_op_ptr rop) { return timer->register_timer(std::move(rop)); }};
    co_await this_coro::switch_to(orig_ex);
    co_return r;
  }

  /// Cancel the pending timer operation.
  void cancel() noexcept {
    auto h = take_handle();
    if (h) {
      h.cancel();
    }
  }

 private:
  auto register_timer(detail::reactor_op_ptr rop) noexcept
    -> detail::io_context_impl::event_handle {
    // SAFETY: `handle_` is the only cancellation hook we have. Hold `mtx_` across both
    // "cancel old handle" and "store new handle" so that `cancel()` cannot observe a
    // partially-registered operation (window between add_timer() and handle_ assignment).
    std::scoped_lock lk{mtx_};
    if (handle_) {
      handle_.cancel();
    }
    auto h = ctx_impl_->add_timer(expiry(), std::move(rop));
    handle_ = h;
    return h;
  }

  auto take_handle() noexcept -> detail::io_context_impl::event_handle {
    std::scoped_lock lk{mtx_};
    return std::exchange(handle_, detail::io_context_impl::event_handle::invalid_handle());
  }

  any_io_executor ex_{};
  detail::io_context_impl* ctx_impl_;
  time_point expiry_{clock::now()};
  mutable std::mutex mtx_{};
  detail::io_context_impl::event_handle handle_{};
};

}  // namespace iocoro
