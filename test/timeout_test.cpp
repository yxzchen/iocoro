#include <iocoro/iocoro.hpp>
#include <iocoro/ip/tcp.hpp>
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <variant>

namespace iocoro::test {

using namespace std::chrono_literals;

auto read_some_with_timeout(ip::tcp::socket& socket,
                            std::span<std::byte> buf,
                            std::chrono::steady_clock::duration timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  auto ex = co_await this_coro::io_executor;

  steady_timer timer(ex);
  timer.expires_after(timeout);

  auto [index, result] =
    co_await (socket.async_read_some(buf) || timer.async_wait(use_awaitable));

  if (index == 0U) {
    co_return std::get<0>(result);
  }

  co_return unexpected(error::timed_out);
}

auto write_with_timeout(ip::tcp::socket& socket,
                        std::span<std::byte const> buf,
                        std::chrono::steady_clock::duration timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  auto ex = co_await this_coro::io_executor;

  steady_timer timer(ex);
  timer.expires_after(timeout);

  auto [index, result] = co_await (io::async_write(socket, buf) || timer.async_wait(use_awaitable));

  if (index == 0U) {
    co_return std::get<0>(result);
  }

  co_return unexpected(error::timed_out);
}

auto connect_expected(ip::tcp::socket& socket, ip::tcp::endpoint const& ep)
  -> awaitable<void_result> {
  co_return co_await socket.async_connect(ep);
}

auto connect_with_timeout(ip::tcp::socket& socket,
                          ip::tcp::endpoint const& ep,
                          std::chrono::steady_clock::duration timeout)
  -> awaitable<void_result> {
  auto ex = co_await this_coro::io_executor;

  steady_timer timer(ex);
  timer.expires_after(timeout);

  auto [index, result] = co_await (connect_expected(socket, ep) || timer.async_wait(use_awaitable));

  if (index == 0U) {
    co_return std::get<0>(result);
  }

  socket.cancel();
  co_return fail(error::timed_out);
}

auto resolve_with_timeout(ip::tcp::resolver& resolver,
                          std::string host,
                          std::string service,
                          std::chrono::steady_clock::duration timeout)
  -> awaitable<expected<ip::tcp::resolver::results_type, std::error_code>> {
  auto ex = co_await this_coro::io_executor;

  steady_timer timer(ex);
  timer.expires_after(timeout);

  auto [index, result] =
    co_await (resolver.async_resolve(std::move(host), std::move(service)) ||
              timer.async_wait(use_awaitable));

  if (index == 0U) {
    co_return std::get<0>(result);
  }

  co_return unexpected(error::timed_out);
}

TEST(timeout_examples, resolve_timeout_microseconds) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto timeout_ec = make_error_code(error::timed_out);
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    thread_pool pool{1};
    pool.get_executor().post([] {
      std::this_thread::sleep_for(5ms);
    });
    ip::tcp::resolver resolver(pool.get_executor());

    auto r = co_await resolve_with_timeout(resolver, "example.com", "80", 1us);
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

    auto r = co_await connect_with_timeout(socket, *ep, 1ms);
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
    auto r = co_await read_some_with_timeout(socket, buf, 1ms);
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
    auto r = co_await write_with_timeout(socket, payload, 1ms);
    timed_out = (!r && r.error() == timeout_ec);
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

}  // namespace iocoro::test
