#include <gtest/gtest.h>

#include <iocoro/iocoro.hpp>
#include <iocoro/thread_pool.hpp>

#include <iocoro/detail/executor_cast.hpp>

#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct switch_to_result {
  bool started_on_ex1{false};
  bool resumed_on_ex2{false};
  std::thread::id tid_before{};
  std::thread::id tid_after{};
};

TEST(switch_to_test, switches_executor_and_thread_pool_thread) {
  iocoro::thread_pool pool1{1};
  iocoro::thread_pool pool2{1};

  auto ex1 = pool1.get_executor();
  auto ex2 = pool2.get_executor();

  std::promise<switch_to_result> done;
  auto fut = done.get_future();

  iocoro::co_spawn(
    ex1,
    [ex1, ex2, done = std::move(done)]() mutable -> iocoro::awaitable<void> {
      switch_to_result r{};

      r.tid_before = std::this_thread::get_id();
      {
        auto cur_any = co_await iocoro::this_coro::executor;
        auto const* cur =
          iocoro::detail::any_executor_access::target<iocoro::thread_pool::executor_type>(
            cur_any);
        r.started_on_ex1 = cur != nullptr && (*cur == ex1);
      }

      co_await iocoro::this_coro::switch_to(iocoro::any_executor{ex2});

      r.tid_after = std::this_thread::get_id();
      {
        auto cur_any = co_await iocoro::this_coro::executor;
        auto const* cur =
          iocoro::detail::any_executor_access::target<iocoro::thread_pool::executor_type>(
            cur_any);
        r.resumed_on_ex2 = cur != nullptr && (*cur == ex2);
      }

      done.set_value(r);
      co_return;
    },
    iocoro::detached);

  ASSERT_EQ(fut.wait_for(2s), std::future_status::ready);
  auto r = fut.get();

  EXPECT_TRUE(r.started_on_ex1);
  EXPECT_TRUE(r.resumed_on_ex2);
  EXPECT_NE(r.tid_before, r.tid_after);
}

}  // namespace


