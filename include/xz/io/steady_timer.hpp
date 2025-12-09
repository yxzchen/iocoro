#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>

#include <chrono>

namespace xz::io {

/// A timer based on steady_clock
class steady_timer {
 public:
  using duration = std::chrono::steady_clock::duration;
  using time_point = std::chrono::steady_clock::time_point;

  explicit steady_timer(io_context& ctx) : ctx_(ctx) {}
  ~steady_timer() = default;

  // Non-copyable, movable
  steady_timer(steady_timer const&) = delete;
  auto operator=(steady_timer const&) -> steady_timer& = delete;
  steady_timer(steady_timer&&) noexcept = default;
  auto operator=(steady_timer&&) noexcept -> steady_timer& = default;

  /// Get the associated io_context
  auto get_executor() noexcept -> io_context& { return ctx_; }

  /// Cancel the timer
  void cancel() {
    if (timer_handle_) {
      ctx_.cancel_timer(timer_handle_);
      timer_handle_.reset();
    }
  }

  /// Async wait operation
  struct [[nodiscard]] async_wait_op : awaitable_op<void> {
    async_wait_op(steady_timer& t, duration d) : timer_(t), duration_(d) {}

   protected:
    void start_operation() override;

   private:
    steady_timer& timer_;
    duration duration_;
  };

  /// Wait for duration
  auto async_wait(duration d) -> async_wait_op {
    return async_wait_op{*this, d};
  }

  /// Wait for duration (convenience)
  template <typename Rep, typename Period>
  auto async_wait(std::chrono::duration<Rep, Period> d) -> async_wait_op {
    return async_wait(std::chrono::duration_cast<duration>(d));
  }

 private:
  friend struct async_wait_op;
  io_context& ctx_;
  detail::timer_handle timer_handle_;
};

}  // namespace xz::io
