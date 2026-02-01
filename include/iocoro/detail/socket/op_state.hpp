#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace iocoro::detail::socket {

struct op_state {
  std::atomic<std::uint64_t> epoch{0};
  bool active{false};

  auto try_start(std::mutex& m, std::uint64_t& epoch_out) -> bool {
    std::scoped_lock lk{m};
    if (active) {
      return false;
    }
    active = true;
    epoch_out = epoch.load(std::memory_order_acquire);
    return true;
  }

  void finish(std::mutex& m) {
    std::scoped_lock lk{m};
    active = false;
  }

  void cancel() noexcept { epoch.fetch_add(1, std::memory_order_acq_rel); }

  auto is_epoch_current(std::uint64_t value) const noexcept -> bool {
    return epoch.load(std::memory_order_acquire) == value;
  }
};

}  // namespace iocoro::detail::socket
