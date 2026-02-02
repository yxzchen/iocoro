#include <gtest/gtest.h>

#include <iocoro/io_context.hpp>
#include <iocoro/ip/tcp.hpp>
#include <iocoro/thread_pool.hpp>

#include "test_util.hpp"

TEST(resolver_test, resolve_with_custom_thread_pool) {
  iocoro::io_context ctx;
  iocoro::thread_pool pool{1};
  iocoro::ip::tcp::resolver resolver{pool.get_executor()};

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::result<iocoro::ip::tcp::resolver::results_type>> {
      co_return co_await resolver.async_resolve("127.0.0.1", "80");
    }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
}

