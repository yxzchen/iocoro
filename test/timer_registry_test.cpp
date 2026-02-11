#include <gtest/gtest.h>

#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/detail/timer_registry.hpp>
#include <iocoro/error.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

struct single_call_state {
  std::atomic<int>* complete_calls{};
  std::atomic<int>* abort_calls{};
  std::atomic<bool>* done{};

  void on_complete() noexcept {
    if (complete_calls != nullptr) {
      complete_calls->fetch_add(1, std::memory_order_relaxed);
    }
    if (done != nullptr) {
      done->store(true, std::memory_order_release);
    }
  }

  void on_abort(std::error_code) noexcept {
    if (abort_calls != nullptr) {
      abort_calls->fetch_add(1, std::memory_order_relaxed);
    }
    if (done != nullptr) {
      done->store(true, std::memory_order_release);
    }
  }
};

inline void abort_and_destroy(iocoro::detail::reactor_op_ptr op) noexcept {
  if (!op) {
    return;
  }
  op->vt->on_abort(op->block, iocoro::error::operation_aborted);
}

}  // namespace

TEST(timer_registry_test, stale_generation_does_not_cancel_new_timer_in_same_slot) {
  iocoro::detail::timer_registry reg;

  std::atomic<int> complete{0};
  std::atomic<int> abort{0};
  std::atomic<bool> done1{false};
  std::atomic<bool> done2{false};

  // Use a near-future expiry so that process_expired() can recycle the node quickly
  // after cancellation, enabling slot reuse.
  auto tok1 = reg.add_timer(std::chrono::steady_clock::now() + std::chrono::milliseconds{1},
                            iocoro::detail::make_reactor_op<single_call_state>(
                              single_call_state{&complete, &abort, &done1}));

  auto cr1 = reg.cancel(tok1);
  ASSERT_TRUE(cr1.cancelled);
  abort_and_destroy(std::move(cr1.op));
  ASSERT_TRUE(done1.load(std::memory_order_acquire));
  EXPECT_EQ(abort.load(std::memory_order_relaxed), 1);

  // Drive the registry so the cancelled node is popped and recycled (generation increments).
  while (!reg.empty()) {
    (void)reg.process_expired();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  // Reuse the freed slot by adding another timer.
  auto tok2 = reg.add_timer(std::chrono::steady_clock::now() + std::chrono::milliseconds{1},
                            iocoro::detail::make_reactor_op<single_call_state>(
                              single_call_state{&complete, &abort, &done2}));
  ASSERT_NE(tok2.generation, tok1.generation);

  // Cancelling with stale generation must not affect the new timer.
  auto stale = reg.cancel(tok1);
  EXPECT_FALSE(stale.cancelled);

  // Drive expiry; it should complete (not abort) exactly once.
  while (!done2.load(std::memory_order_acquire)) {
    (void)reg.process_expired();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  EXPECT_EQ(complete.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(abort.load(std::memory_order_relaxed), 1);
}
