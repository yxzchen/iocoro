#include <gtest/gtest.h>

#include <iocoro/error.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip/tcp.hpp>

#include "test_util.hpp"

#include <atomic>
#include <memory>
#include <stop_token>

namespace {

struct counting_executor {
  std::shared_ptr<std::atomic<int>> post_count{};

  void post(iocoro::detail::unique_function<void()> fn) const noexcept {
    post_count->fetch_add(1, std::memory_order_relaxed);
    fn();
  }

  void dispatch(iocoro::detail::unique_function<void()> fn) const noexcept { post(std::move(fn)); }

  friend auto operator==(counting_executor const& a, counting_executor const& b) noexcept -> bool {
    return a.post_count == b.post_count;
  }
};

}  // namespace

TEST(ip_resolver_test, resolve_cancelled_before_call_does_not_queue_pool_work) {
  iocoro::io_context ctx;
  auto post_count = std::make_shared<std::atomic<int>>(0);
  iocoro::ip::tcp::resolver resolver{counting_executor{post_count}};

  std::stop_source stop_src{};
  stop_src.request_stop();

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(
           ctx.get_executor(), stop_src.get_token(),
           [&]() -> iocoro::awaitable<iocoro::result<iocoro::ip::tcp::resolver::results_type>> {
             co_return co_await resolver.async_resolve("127.0.0.1", "80");
           }(),
           iocoro::use_awaitable));

  ASSERT_TRUE(r);
  ASSERT_FALSE(static_cast<bool>(*r));
  EXPECT_EQ(r->error(), iocoro::error::operation_aborted);
  EXPECT_EQ(post_count->load(std::memory_order_relaxed), 0);
}
