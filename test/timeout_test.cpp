#include <iocoro/iocoro.hpp>
#include <iocoro/ip/tcp.hpp>
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace iocoro::test {

using namespace std::chrono_literals;

TEST(timeout_examples, resolve_timeout_microseconds) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto timeout_ec = make_error_code(error::timed_out);
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    thread_pool pool{1};
    pool.get_executor().post([] { std::this_thread::sleep_for(5ms); });
    ip::tcp::resolver resolver(pool.get_executor());

    auto r = co_await with_timeout(resolver.async_resolve("example.com", "80"), 1us);
    timed_out = (!r && r.error() == timeout_ec);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

TEST(timeout_examples, connect_timeout_ms) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto timeout_ec = make_error_code(error::timed_out);
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    ip::tcp::socket socket(ex);

    auto ep = ip::tcp::endpoint::from_string("58.246.163.58:80");
    if (!ep) {
      throw std::runtime_error("invalid endpoint");
    }

    auto r = co_await with_timeout(socket.async_connect(*ep), 1ms);
    timed_out = (!r && r.error() == timeout_ec);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

TEST(timeout_examples, read_timeout_ms) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto timeout_ec = make_error_code(error::timed_out);
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    ip::tcp::socket socket(ex);

    auto ep = ip::tcp::endpoint::from_string("58.246.163.58:80");
    if (!ep) {
      throw std::runtime_error("invalid endpoint");
    }

    auto cr = co_await socket.async_connect(*ep);
    if (!cr) {
      co_return;
    }

    std::array<std::byte, 1024> buf{};
    auto r = co_await with_timeout(socket.async_read_some(buf), 1ms);
    timed_out = (!r && r.error() == timeout_ec);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

TEST(timeout_examples, write_timeout_ms) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto timeout_ec = make_error_code(error::timed_out);
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    ip::tcp::socket socket(ex);

    auto ep = ip::tcp::endpoint::from_string("58.246.163.58:80");
    if (!ep) {
      throw std::runtime_error("invalid endpoint");
    }

    auto cr = co_await socket.async_connect(*ep);
    if (!cr) {
      co_return;
    }

    std::vector<std::byte> payload(16 * 1024 * 1024);
    auto r = co_await with_timeout(io::async_write(socket, payload), 1ms);
    timed_out = (!r && r.error() == timeout_ec);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

}  // namespace iocoro::test
