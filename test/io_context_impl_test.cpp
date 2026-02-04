#include <gtest/gtest.h>

#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_types.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>
 
#include <cstddef>
#include <functional>

using namespace std::chrono_literals;

namespace {

struct noop_state {
  void on_complete() noexcept {}
  void on_abort(std::error_code) noexcept {}
};

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

TEST(io_context_impl_test, post_and_run_executes_operations) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<int> count{0};
  ctx->post([&] { ++count; });
  ctx->post([&] { ++count; });

  ctx->run();
  EXPECT_EQ(count.load(), 2);
}

TEST(io_context_impl_test, run_one_processes_single_task) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<int> count{0};
  ctx->post([&] {
    ++count;
    ctx->post([&] { ++count; });
  });

  ctx->run_one();
  EXPECT_EQ(count.load(), 1);

  ctx->run_one();
  EXPECT_EQ(count.load(), 2);
}

TEST(io_context_impl_test, run_for_without_work_returns_zero) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();
  auto n = ctx->run_for(std::chrono::milliseconds{1});
  EXPECT_EQ(n, 0U);
}

TEST(io_context_impl_test, schedule_timer_executes_callback) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<bool> fired{false};
  std::atomic<bool> aborted{false};

  struct timer_state {
    std::atomic<bool>* fired;
    std::atomic<bool>* aborted;

    void on_complete() noexcept { fired->store(true); }
    void on_abort(std::error_code) noexcept { aborted->store(true); }
  };

  auto op = iocoro::detail::make_reactor_op<timer_state>(&fired, &aborted);
  (void)ctx->add_timer(std::chrono::steady_clock::now(), std::move(op));

  ctx->run_one();
  EXPECT_TRUE(fired.load());
  EXPECT_FALSE(aborted.load());
}

TEST(io_context_impl_test, dispatch_runs_inline_on_context_thread) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::vector<int> order;
  ctx->post([&] {
    order.push_back(1);
    ctx->dispatch([&] { order.push_back(2); });
    order.push_back(3);
  });

  ctx->run();
  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

TEST(io_context_impl_test, run_for_processes_posted_work) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<int> count{0};
  ctx->post([&] { ++count; });

  auto n = ctx->run_for(std::chrono::milliseconds{1});
  EXPECT_EQ(n, 1U);
  EXPECT_EQ(count.load(), 1);
}

TEST(io_context_impl_test, concurrent_run_is_rejected) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

  EXPECT_DEATH(
    {
      auto impl = std::make_shared<iocoro::detail::io_context_impl>();
      impl->add_work_guard();
      std::jthread runner{[&](std::stop_token) { (void)impl->run_for(1s); }};
      std::this_thread::sleep_for(5ms);
      (void)impl->run_one();
    },
    ".*");
}

TEST(io_context_impl_test, add_timer_wrong_thread_when_running_is_rejected) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

  EXPECT_DEATH(
    {
      auto impl = std::make_shared<iocoro::detail::io_context_impl>();
      impl->add_work_guard();
      std::jthread runner{[&](std::stop_token) { (void)impl->run_for(1s); }};
      std::this_thread::sleep_for(5ms);
      auto op = iocoro::detail::make_reactor_op<noop_state>();
      (void)impl->add_timer(std::chrono::steady_clock::now(), std::move(op));
    },
    ".*");
}

TEST(io_context_impl_test, cancel_timer_requires_shared_ownership) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

  EXPECT_DEATH(
    {
      // Intentionally violate the contract: io_context_impl must be shared-owned.
      iocoro::detail::io_context_impl impl{};
      impl.cancel_timer(/*index=*/1U, /*generation=*/1U);
    },
    ".*");
}

TEST(io_context_impl_test, stress_cancel_timer_from_foreign_thread_does_not_invoke_abort_inline) {
  auto impl = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<std::size_t> abort_tid{0};
  std::atomic<int> abort_calls{0};
  std::atomic<int> complete_calls{0};

  auto op = iocoro::detail::make_reactor_op<record_abort_thread_state>(
    record_abort_thread_state{&abort_tid, &abort_calls, &complete_calls});

  auto h =
    impl->add_timer(std::chrono::steady_clock::now() + std::chrono::seconds{10}, std::move(op));
  ASSERT_EQ(abort_calls.load(std::memory_order_relaxed), 0);
  ASSERT_EQ(complete_calls.load(std::memory_order_relaxed), 0);

  std::thread canceller([&] { impl->cancel_timer(h.timer_index, h.timer_generation); });
  canceller.join();

  EXPECT_EQ(abort_calls.load(std::memory_order_relaxed), 0);

  auto const run_tid = thread_hash();
  impl->run_one();

  ASSERT_EQ(abort_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(abort_tid.load(std::memory_order_relaxed), run_tid);
  EXPECT_EQ(complete_calls.load(std::memory_order_relaxed), 0);
}
