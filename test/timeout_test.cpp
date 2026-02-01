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
#include <system_error>
#include <vector>
#include <variant>

namespace iocoro::test {

using namespace std::chrono_literals;

auto read_some_with_timeout(ip::tcp::socket& socket,
                            std::span<std::byte> buf,
                            std::chrono::steady_clock::duration timeout)
  -> awaitable<std::size_t> {
  auto ex = co_await this_coro::io_executor;

  steady_timer timer(ex);
  timer.expires_after(timeout);

  auto [index, result] =
    co_await (socket.async_read_some(buf) || timer.async_wait(use_awaitable));

  if (index == 0U) {
    auto r = std::get<0>(result);
    if (!r) {
      throw std::system_error(r.error());
    }
    co_return *r;
  }

  throw std::runtime_error("read timeout");
}

auto write_with_timeout(ip::tcp::socket& socket,
                        std::span<std::byte const> buf,
                        std::chrono::steady_clock::duration timeout) -> awaitable<void> {
  auto ex = co_await this_coro::io_executor;

  steady_timer timer(ex);
  timer.expires_after(timeout);

  auto [index, result] = co_await (io::async_write(socket, buf) || timer.async_wait(use_awaitable));

  if (index == 0U) {
    auto r = std::get<0>(result);
    if (!r) {
      throw std::system_error(r.error());
    }
    co_return;
  }

  throw std::runtime_error("write timeout");
}

auto connect_with_timeout(ip::tcp::socket& socket,
                          ip::tcp::endpoint const& ep,
                          std::chrono::steady_clock::duration timeout) -> awaitable<void> {
  auto ex = co_await this_coro::io_executor;

  steady_timer timer(ex);
  timer.expires_after(timeout);

  auto connect_task = [&socket, &ep]() -> awaitable<std::monostate> {
    auto ec = co_await socket.async_connect(ep);
    if (ec) {
      throw std::system_error(ec);
    }
    co_return std::monostate{};
  };

  auto [index, result] = co_await (connect_task() || timer.async_wait(use_awaitable));

  if (index == 0U) {
    co_return;
  }

  socket.cancel();
  throw std::runtime_error("connect timeout");
}

auto resolve_with_timeout(ip::tcp::resolver& resolver,
                          std::string host,
                          std::string service,
                          std::chrono::steady_clock::duration timeout)
  -> awaitable<ip::tcp::resolver::results_type> {
  auto ex = co_await this_coro::io_executor;

  steady_timer timer(ex);
  timer.expires_after(timeout);

  auto [index, result] =
    co_await (resolver.async_resolve(std::move(host), std::move(service)) ||
              timer.async_wait(use_awaitable));

  if (index == 0U) {
    auto r = std::get<0>(result);
    if (!r) {
      throw std::system_error(r.error());
    }
    co_return std::move(*r);
  }

  throw std::runtime_error("resolve timeout");
}

TEST(timeout_examples, resolve_timeout_microseconds) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    ip::tcp::resolver resolver(ex);

    try {
      (void)co_await resolve_with_timeout(resolver, "example.com", "80", 1us);
    } catch (std::runtime_error const&) {
      timed_out = true;
    }
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

TEST(timeout_examples, connect_timeout_ms) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    ip::tcp::socket socket(ex);

    auto ep = ip::tcp::endpoint::from_string("58.246.163.58:80");
    if (!ep) {
      throw std::runtime_error("invalid endpoint");
    }

    try {
      co_await connect_with_timeout(socket, *ep, 1ms);
    } catch (std::runtime_error const&) {
      timed_out = true;
    }
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

TEST(timeout_examples, read_timeout_ms) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    ip::tcp::socket socket(ex);

    auto ep = ip::tcp::endpoint::from_string("58.246.163.58:80");
    if (!ep) {
      throw std::runtime_error("invalid endpoint");
    }

    auto ec = co_await socket.async_connect(*ep);
    if (ec) {
      throw std::system_error(ec);
    }

    std::array<std::byte, 1024> buf{};
    try {
      (void)co_await read_some_with_timeout(socket, buf, 1ms);
    } catch (std::runtime_error const&) {
      timed_out = true;
    }
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

TEST(timeout_examples, write_timeout_ms) {
  iocoro::io_context ctx;

  bool timed_out = false;
  auto task = [&]() -> awaitable<void> {
    auto ex = co_await this_coro::io_executor;
    ip::tcp::socket socket(ex);

    auto ep = ip::tcp::endpoint::from_string("58.246.163.58:80");
    if (!ep) {
      throw std::runtime_error("invalid endpoint");
    }

    auto ec = co_await socket.async_connect(*ep);
    if (ec) {
      throw std::system_error(ec);
    }

    std::vector<std::byte> payload(16 * 1024 * 1024);
    try {
      co_await write_with_timeout(socket, payload, 1ms);
    } catch (std::runtime_error const&) {
      timed_out = true;
    }
    co_return;
  };

  auto r = iocoro::test::sync_wait(ctx, task());
  ASSERT_TRUE(r);
  EXPECT_TRUE(timed_out);
}

}  // namespace iocoro::test
