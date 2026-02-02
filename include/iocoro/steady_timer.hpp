#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_awaiter.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
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
  explicit steady_timer(any_io_executor ex, time_point at) noexcept
      : ctx_impl_(ex.io_context_ptr()), expiry_(at) {}
  explicit steady_timer(any_io_executor ex, duration after) noexcept
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
  /// - `ok()` on successful timer expiry.
  auto async_wait(use_awaitable_t) -> awaitable<result<void>> {
    auto* timer = this;
    auto ec = co_await detail::operation_awaiter{
      [timer](detail::reactor_op_ptr rop) {
        return timer->register_timer(std::move(rop));
      }};
    if (ec) {
      co_return fail(ec);
    }
    co_return ok();
  }

  /// Cancel the pending timer operation.
  void cancel() noexcept {
    auto h = take_handle();
    if (h) {
      h.cancel();
    }
  }

  time_point expiry() { return expiry_; }

 private:
  auto register_timer(detail::reactor_op_ptr rop) noexcept -> detail::io_context_impl::event_handle {
    if (!ctx_impl_) {
      return detail::io_context_impl::event_handle::invalid_handle();
    }
    auto h = ctx_impl_->add_timer(expiry(), std::move(rop));
    set_handle(h);
    return h;
  }

  void set_handle(detail::io_context_impl::event_handle h) noexcept {
    std::scoped_lock lk{mtx_};
    handle_ = h;
  }

  auto take_handle() noexcept -> detail::io_context_impl::event_handle {
    std::scoped_lock lk{mtx_};
    return std::exchange(handle_, detail::io_context_impl::event_handle::invalid_handle());
  }

  detail::io_context_impl* ctx_impl_;
  time_point expiry_{clock::now()};
  mutable std::mutex mtx_{};
  detail::io_context_impl::event_handle handle_{};
};

}  // namespace iocoro
