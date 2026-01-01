#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace iocoro::detail {

struct operation_base;

enum class timer_state : std::uint8_t {
  pending,
  fired,
  cancelled,
};

struct timer_entry {
  std::uint64_t id{};
  std::chrono::steady_clock::time_point expiry;
  std::unique_ptr<operation_base> op;

  std::atomic<timer_state> state{timer_state::pending};

  timer_entry() = default;

  timer_entry(const timer_entry&) = delete;
  auto operator=(const timer_entry&) -> timer_entry& = delete;
  timer_entry(timer_entry&&) = delete;
  auto operator=(timer_entry&&) -> timer_entry& = delete;

  auto is_pending() const noexcept -> bool {
    return state.load(std::memory_order_acquire) == timer_state::pending;
  }

  auto is_cancelled() const noexcept -> bool {
    return state.load(std::memory_order_acquire) == timer_state::cancelled;
  }

  auto mark_fired() noexcept -> bool {
    auto expected = timer_state::pending;
    return state.compare_exchange_strong(
      expected, timer_state::fired,
      std::memory_order_acq_rel, std::memory_order_acquire
    );
  }

  auto cancel() noexcept -> bool {
    auto expected = timer_state::pending;
    return state.compare_exchange_strong(
      expected, timer_state::cancelled,
      std::memory_order_acq_rel, std::memory_order_acquire
    );
  }
};

}  // namespace iocoro::detail
