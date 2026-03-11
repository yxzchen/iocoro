#pragma once

#include <atomic>

namespace iocoro::detail {

class wake_state {
 public:
  // `running`: reactor thread is not committed to block in wait().
  // `waiting`: reactor thread is about to block or is blocked in wait().
  // `notified`: a wake token has been reserved for this wait window.
  enum class phase : unsigned char { running, waiting, notified };

  void begin_wait() noexcept {
    auto expected = phase::running;
    (void)phase_.compare_exchange_strong(expected, phase::waiting, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
  }

  auto notify() noexcept -> bool {
    auto current = phase_.load(std::memory_order_acquire);
    for (;;) {
      if (current != phase::waiting) {
        return false;
      }
      if (phase_.compare_exchange_weak(current, phase::notified, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return true;
      }
    }
  }

  // The caller claimed responsibility for notifying the kernel but the write
  // failed before the kernel observed it. Keep the current wait window open so
  // a later notify() can retry.
  void notify_failed() noexcept { phase_.store(phase::waiting, std::memory_order_release); }

  void consume_notification() noexcept { phase_.store(phase::running, std::memory_order_release); }

  // wait() returned without draining the wake fd. Only close the window if no
  // notify raced and moved the state to `notified`.
  void finish_wait() noexcept {
    auto current = phase_.load(std::memory_order_acquire);
    while (current == phase::waiting &&
           !phase_.compare_exchange_weak(current, phase::running, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
    }
  }

  auto current_phase() const noexcept -> phase { return phase_.load(std::memory_order_acquire); }

 private:
  std::atomic<phase> phase_{phase::running};
};

}  // namespace iocoro::detail
