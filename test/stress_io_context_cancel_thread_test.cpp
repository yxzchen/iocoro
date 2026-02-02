#include <gtest/gtest.h>

#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>

namespace {

inline auto thread_hash() -> std::size_t {
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

struct record_abort_thread_state {
  std::atomic<std::size_t>* abort_thread{};
  std::atomic<int>* abort_calls{};
  std::atomic<int>* complete_calls{};

  void on_complete() noexcept {
    if (complete_calls != nullptr) {
      complete_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }

  void on_abort(std::error_code) noexcept {
    if (abort_thread != nullptr) {
      abort_thread->store(thread_hash(), std::memory_order_relaxed);
    }
    if (abort_calls != nullptr) {
      abort_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }
};

}  // namespace

TEST(stress_io_context_cancel_thread, cancel_timer_from_foreign_thread_does_not_invoke_abort_inline) {
  // This test encodes the intended v2 invariant:
  // cancellation callbacks must execute on the reactor thread (the thread calling run()).
  //
  // Current implementation may execute abort inline when the loop is not running.
  // The plan requires hardening this behavior; this test should pass after that refactor.

  iocoro::detail::io_context_impl impl;

  std::atomic<std::size_t> abort_tid{0};
  std::atomic<int> abort_calls{0};
  std::atomic<int> complete_calls{0};

  auto op = iocoro::detail::make_reactor_op<record_abort_thread_state>(
    record_abort_thread_state{&abort_tid, &abort_calls, &complete_calls});

  auto h =
    impl.add_timer(std::chrono::steady_clock::now() + std::chrono::seconds{10}, std::move(op));
  ASSERT_EQ(abort_calls.load(std::memory_order_relaxed), 0);
  ASSERT_EQ(complete_calls.load(std::memory_order_relaxed), 0);

  std::thread canceller([&] {
    // Cancel before the loop starts.
    impl.cancel_timer(h.timer_index, h.timer_generation);
  });
  canceller.join();

  // Under the intended invariant, cancellation should be deferred until run() establishes the
  // reactor thread, so abort shouldn't have executed yet.
  EXPECT_EQ(abort_calls.load(std::memory_order_relaxed), 0);

  auto const run_tid = thread_hash();
  impl.run_one();

  ASSERT_EQ(abort_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(abort_tid.load(std::memory_order_relaxed), run_tid);
  EXPECT_EQ(complete_calls.load(std::memory_order_relaxed), 0);
}

