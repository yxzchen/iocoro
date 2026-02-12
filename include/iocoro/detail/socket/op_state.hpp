#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace iocoro::detail::socket {

struct op_state {
  std::atomic<std::uint64_t> epoch{0};
  std::atomic<bool> active{false};

  auto try_start(std::uint64_t& epoch_out) noexcept -> bool {
    bool expected = false;
    if (!active.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
      return false;
    }
    epoch_out = epoch.load(std::memory_order_acquire);
    return true;
  }

  void finish() noexcept { active.store(false, std::memory_order_release); }

  auto is_active() const noexcept -> bool { return active.load(std::memory_order_acquire); }

  void cancel() noexcept { epoch.fetch_add(1, std::memory_order_acq_rel); }

  auto is_epoch_current(std::uint64_t value) const noexcept -> bool {
    return epoch.load(std::memory_order_acquire) == value;
  }
};

}  // namespace iocoro::detail::socket
