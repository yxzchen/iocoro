#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>

#include <chrono>
#include <memory>

namespace xz::io {

namespace detail {
class steady_timer_impl;
}

/// A timer based on steady_clock
class steady_timer {
 public:
  using duration = std::chrono::steady_clock::duration;
  using time_point = std::chrono::steady_clock::time_point;

  explicit steady_timer(io_context& ctx);
  ~steady_timer();

  // Non-copyable, movable
  steady_timer(steady_timer const&) = delete;
  auto operator=(steady_timer const&) -> steady_timer& = delete;
  steady_timer(steady_timer&&) noexcept;
  auto operator=(steady_timer&&) noexcept -> steady_timer&;

  /// Get the associated io_context
  auto get_executor() noexcept -> io_context&;

  /// Cancel the timer
  void cancel();

  /// Async wait operation
  struct [[nodiscard]] async_wait_op : awaitable_op<void> {
    async_wait_op(steady_timer& t, duration d);

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
  std::unique_ptr<detail::steady_timer_impl> impl_;
};

}  // namespace xz::io
