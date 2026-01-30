#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace iocoro::detail {

template <class T, std::size_t Capacity>
class lockfree_mpmc_queue {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");

 public:
  lockfree_mpmc_queue() noexcept {
    for (std::size_t i = 0; i < Capacity; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  lockfree_mpmc_queue(lockfree_mpmc_queue const&) = delete;
  auto operator=(lockfree_mpmc_queue const&) -> lockfree_mpmc_queue& = delete;

  auto try_enqueue(T& value) noexcept -> bool {
    cell* c = nullptr;
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      c = &buffer_[pos & mask_];
      std::size_t const seq = c->sequence.load(std::memory_order_acquire);
      std::intptr_t const diff = static_cast<std::intptr_t>(seq) -
                                 static_cast<std::intptr_t>(pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(
              pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }

    c->data = std::move(value);
    c->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  auto try_dequeue(T& out) noexcept -> bool {
    cell* c = nullptr;
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      c = &buffer_[pos & mask_];
      std::size_t const seq = c->sequence.load(std::memory_order_acquire);
      std::intptr_t const diff = static_cast<std::intptr_t>(seq) -
                                 static_cast<std::intptr_t>(pos + 1);
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(
              pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }

    out = std::move(c->data);
    c->sequence.store(pos + Capacity, std::memory_order_release);
    return true;
  }

 private:
  struct cell {
    std::atomic<std::size_t> sequence{};
    T data{};
  };

  static constexpr std::size_t mask_ = Capacity - 1;
  alignas(64) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(64) std::atomic<std::size_t> dequeue_pos_{0};
  std::array<cell, Capacity> buffer_{};
};

}  // namespace iocoro::detail
