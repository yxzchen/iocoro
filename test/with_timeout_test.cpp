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

auto immediate_int(int v) -> iocoro::awaitable<int> {
  co_return v;
}

auto immediate_void() -> iocoro::awaitable<void> {
  co_return;
}

auto throw_runtime_error() -> iocoro::awaitable<int> {
  throw std::runtime_error("boom");
  co_return 0;
}

auto throw_runtime_error_void() -> iocoro::awaitable<void> {
  throw std::runtime_error("boom");
  co_return;
}

auto wait_timer_value(iocoro::steady_timer& timer,
                      std::shared_ptr<std::optional<std::error_code>> out, int value)
  -> iocoro::awaitable<int> {
  auto r = co_await timer.async_wait(iocoro::use_awaitable);
  *out = r ? std::error_code{} : r.error();
  co_return value;
}

auto wait_timer_ec(iocoro::steady_timer& timer, std::shared_ptr<std::optional<std::error_code>> out)
  -> iocoro::awaitable<std::error_code> {
  auto r = co_await timer.async_wait(iocoro::use_awaitable);
  auto ec = r ? std::error_code{} : r.error();
  *out = ec;
  co_return ec;
}

auto wait_timer_void(iocoro::steady_timer& timer,
                     std::shared_ptr<std::optional<std::error_code>> out)
  -> iocoro::awaitable<void> {
  auto r = co_await timer.async_wait(iocoro::use_awaitable);
  *out = r ? std::error_code{} : r.error();
  co_return;
}

auto long_timer_wait_ec(std::chrono::steady_clock::duration d,
                        std::shared_ptr<std::optional<std::error_code>> out)
  -> iocoro::awaitable<std::error_code> {
  auto ex = co_await iocoro::this_coro::io_executor;
  iocoro::steady_timer t(ex);
  t.expires_after(d);
  co_return co_await wait_timer_ec(t, out);
}

auto blocking_sleep_set_flag(std::chrono::milliseconds d, std::shared_ptr<std::atomic<bool>> done,
                             int value) -> iocoro::awaitable<int> {
  std::this_thread::sleep_for(d);
  done->store(true);
  co_return value;
}

auto blocking_sleep_then_throw(std::chrono::milliseconds d) -> iocoro::awaitable<int> {
  std::this_thread::sleep_for(d);
  throw std::runtime_error("boom");
  co_return 0;
}

TEST(with_timeout_test, timer_race_cancels_loser) {
  iocoro::io_context ctx;

  auto fast_ec = std::make_shared<std::optional<std::error_code>>();
  auto slow_ec = std::make_shared<std::optional<std::error_code>>();

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;

    iocoro::steady_timer fast_timer(ex);
    fast_timer.expires_after(5ms);

    iocoro::steady_timer slow_timer(ex);
    slow_timer.expires_after(50ms);

    auto [index, result] = co_await iocoro::when_any_cancel_join(
      wait_timer_value(fast_timer, fast_ec, 7), wait_timer_ec(slow_timer, slow_ec));

    EXPECT_EQ(index, 0U);
    EXPECT_EQ(std::get<0>(result), 7);
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);

  auto aborted = iocoro::make_error_code(iocoro::error::operation_aborted);
  ASSERT_TRUE(fast_ec->has_value());
  EXPECT_EQ(fast_ec->value(), std::error_code{});
  ASSERT_TRUE(slow_ec->has_value());
  EXPECT_EQ(slow_ec->value(), aborted);
}

TEST(with_timeout_test, operator_or_returns_second_when_second_wins) {
  iocoro::io_context ctx;

  auto a_ec = std::make_shared<std::optional<std::error_code>>();
  auto b_ec = std::make_shared<std::optional<std::error_code>>();

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;

    iocoro::steady_timer slow(ex);
    slow.expires_after(50ms);

    iocoro::steady_timer fast(ex);
    fast.expires_after(5ms);

    auto [index, result] = co_await iocoro::when_any_cancel_join(wait_timer_ec(slow, a_ec),
                                                                 wait_timer_value(fast, b_ec, 9));

    EXPECT_EQ(index, 1U);
    EXPECT_EQ(std::get<1>(result), 9);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);

  auto aborted = iocoro::make_error_code(iocoro::error::operation_aborted);
  ASSERT_TRUE(a_ec->has_value());
  EXPECT_EQ(a_ec->value(), aborted);
  ASSERT_TRUE(b_ec->has_value());
  EXPECT_EQ(b_ec->value(), std::error_code{});
}

TEST(with_timeout_test, operator_or_supports_void_and_value) {
  iocoro::io_context ctx;

  auto a_ec = std::make_shared<std::optional<std::error_code>>();
  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;
    iocoro::steady_timer fast(ex);
    fast.expires_after(5ms);

    iocoro::steady_timer slow(ex);
    slow.expires_after(50ms);

    auto [index, result] =
      co_await iocoro::when_any_cancel_join(wait_timer_void(fast, a_ec),
                                            wait_timer_value(slow, a_ec, 3));
    EXPECT_EQ(index, 0U);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(result));
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(with_timeout_test, operator_or_rethrows_if_first_completion_throws_and_requests_stop) {
  iocoro::io_context ctx;

  auto loser_ec = std::make_shared<std::optional<std::error_code>>();
  auto saw_exception = std::make_shared<std::atomic<bool>>(false);

  auto task = [&]() -> iocoro::awaitable<void> {
    try {
      (void)co_await iocoro::when_any_cancel_join(throw_runtime_error(),
                                                  long_timer_wait_ec(1h, loser_ec));
    } catch (std::runtime_error const&) {
      saw_exception->store(true);
    }

    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);

  EXPECT_TRUE(saw_exception->load());
  ASSERT_TRUE(loser_ec->has_value());
  EXPECT_EQ(loser_ec->value(), iocoro::make_error_code(iocoro::error::operation_aborted));
}

TEST(with_timeout_test, operator_or_ignores_loser_exception_after_winner_completes) {
  iocoro::io_context ctx;
  iocoro::thread_pool pool{1};

  auto completed = std::make_shared<std::atomic<bool>>(false);
  auto task = [&]() -> iocoro::awaitable<void> {
    auto loser = iocoro::bind_executor(iocoro::any_executor{pool.get_executor()},
                                       blocking_sleep_then_throw(30ms));
    auto [index, result] = co_await iocoro::when_any_cancel_join(immediate_int(7), std::move(loser));
    EXPECT_EQ(index, 0U);
    EXPECT_EQ(std::get<0>(result), 7);
    completed->store(true);
    co_await iocoro::co_sleep(50ms);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(completed->load());
  pool.join();
}

TEST(with_timeout_test, when_any_cancel_join_joins_loser_before_return) {
  iocoro::io_context ctx;
  iocoro::thread_pool pool{1};

  auto loser_done = std::make_shared<std::atomic<bool>>(false);

  auto task = [&]() -> iocoro::awaitable<void> {
    auto a = immediate_int(1);
    auto b = iocoro::bind_executor(iocoro::any_executor{pool.get_executor()},
                                   blocking_sleep_set_flag(80ms, loser_done, 2));

    auto [index, result] = co_await iocoro::when_any_cancel_join(std::move(a), std::move(b));
    EXPECT_EQ(index, 0U);
    EXPECT_EQ(std::get<0>(result), 1);

    // Joined: b must have finished before return.
    EXPECT_TRUE(loser_done->load());
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(with_timeout_test, when_any_cancel_join_joins_loser_even_if_winner_throws) {
  iocoro::io_context ctx;
  iocoro::thread_pool pool{1};

  auto loser_done = std::make_shared<std::atomic<bool>>(false);
  auto saw_exception = std::make_shared<std::atomic<bool>>(false);

  auto task = [&]() -> iocoro::awaitable<void> {
    auto a = throw_runtime_error();
    auto b = iocoro::bind_executor(iocoro::any_executor{pool.get_executor()},
                                   blocking_sleep_set_flag(80ms, loser_done, 2));

    try {
      (void)co_await iocoro::when_any_cancel_join(std::move(a), std::move(b));
    } catch (std::runtime_error const&) {
      saw_exception->store(true);
    }

    EXPECT_TRUE(loser_done->load());
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(saw_exception->load());
}

TEST(with_timeout_test, when_any_cancel_join_cancels_timer_loser_and_waits_for_completion) {
  iocoro::io_context ctx;

  auto loser_ec = std::make_shared<std::optional<std::error_code>>();
  auto task = [&]() -> iocoro::awaitable<void> {
    auto [index, result] =
      co_await iocoro::when_any_cancel_join(immediate_int(7), long_timer_wait_ec(1h, loser_ec));
    EXPECT_EQ(index, 0U);
    EXPECT_EQ(std::get<0>(result), 7);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);

  ASSERT_TRUE(loser_ec->has_value());
  EXPECT_EQ(loser_ec->value(), iocoro::make_error_code(iocoro::error::operation_aborted));
}

TEST(with_timeout_test, operator_or_allows_either_winner_when_both_complete_similarly) {
  iocoro::io_context ctx;

  auto task = [&]() -> iocoro::awaitable<void> {
    auto ex = co_await iocoro::this_coro::io_executor;

    iocoro::steady_timer t1(ex);
    t1.expires_after(5ms);
    iocoro::steady_timer t2(ex);
    t2.expires_after(5ms);

    auto [index, result] = co_await iocoro::when_any_cancel_join(
      wait_timer_value(t1, std::make_shared<std::optional<std::error_code>>(), 1),
      wait_timer_value(t2, std::make_shared<std::optional<std::error_code>>(), 2));

    EXPECT_TRUE(index == 0U || index == 1U);
    if (index == 0U) {
      EXPECT_EQ(std::get<0>(result), 1);
    } else {
      EXPECT_EQ(std::get<1>(result), 2);
    }
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
}

TEST(with_timeout_test, resolve_timeout_microseconds) {
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

TEST(with_timeout_test, connect_timeout_ms) {
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

TEST(with_timeout_test, read_timeout_ms) {
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

TEST(with_timeout_test, write_timeout_ms) {
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

TEST(with_timeout_test, stop_returns_operation_aborted) {
  iocoro::io_context ctx;

  std::stop_source stop_src{};
  auto aborted_ec = make_error_code(error::operation_aborted);

  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;

    steady_timer t(ex);
    t.expires_after(24h);

    auto r = co_await with_timeout(t.async_wait(use_awaitable), 24h);
    if (r) {
      ADD_FAILURE() << "expected operation_aborted, got success";
    } else {
      EXPECT_EQ(r.error(), aborted_ec);
    }
    co_return;
  };

  std::jthread stopper{[&]() {
    std::this_thread::sleep_for(1ms);
    stop_src.request_stop();
  }};

  auto r = iocoro::test::sync_wait(
    ctx, iocoro::co_spawn(ctx.get_executor(), stop_src.get_token(), task(), iocoro::use_awaitable));
  ASSERT_TRUE(r);
}

}  // namespace iocoro::test
