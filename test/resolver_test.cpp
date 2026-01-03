#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip.hpp>
#include <iocoro/thread_pool.hpp>

#include "test_util.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

namespace {

using namespace std::chrono_literals;

// Helper to check if we should skip network-dependent tests.
static auto should_skip_network_test(std::error_code ec) -> bool {
  // EAI_* errors from getaddrinfo are in generic_category for now.
  // Skip tests on network unreachable, DNS unavailable, etc.
  return ec == std::errc::network_unreachable || ec == std::errc::host_unreachable ||
         ec.value() == -2 ||  // EAI_NONAME (name or service not known)
         ec.value() == -3;    // EAI_AGAIN (temporary failure)
}

TEST(resolver_test, resolve_with_default_pool) {
  auto ctx = iocoro::io_context{};

  struct test_result {
    std::optional<std::error_code> error;
    std::size_t count = 0;
  };

  auto result = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<test_result> {
    test_result r;
    // Use default internal thread_pool (no pool executor provided).
    auto resolver = iocoro::ip::tcp::resolver{};
    auto resolve_result = co_await resolver.async_resolve("127.0.0.1", "80");

    if (!resolve_result) {
      r.error = resolve_result.error();
      co_return r;
    }

    r.count = resolve_result->size();
    co_return r;
  }());

  if (result.error) {
    FAIL() << "Resolve failed: " << result.error->message();
  }

  EXPECT_EQ(result.count, 1u);
}

TEST(resolver_test, resolve_localhost_ipv4) {
  auto ctx = iocoro::io_context{};
  auto pool = iocoro::thread_pool{2};

  struct test_result {
    std::optional<std::error_code> error;
    std::size_t count = 0;
    bool has_loopback = false;
  };

  auto result = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<test_result> {
    test_result r;
    auto resolver = iocoro::ip::tcp::resolver{pool.get_executor()};
    auto resolve_result = co_await resolver.async_resolve("localhost", "80");

    if (!resolve_result) {
      r.error = resolve_result.error();
      co_return r;
    }

    r.count = resolve_result->size();

    for (auto const& ep : *resolve_result) {
      auto addr = ep.address();
      if (addr.is_v4() && addr.to_v4().is_loopback()) {
        r.has_loopback = true;
        EXPECT_EQ(ep.port(), 80);
      }
    }

    co_return r;
  }());

  pool.stop();
  pool.join();

  if (result.error) {
    if (should_skip_network_test(*result.error)) {
      GTEST_SKIP() << "Network unavailable: " << result.error->message();
    }
    FAIL() << "Resolve failed: " << result.error->message();
  }

  EXPECT_GT(result.count, 0u);
  EXPECT_TRUE(result.has_loopback);
}

TEST(resolver_test, resolve_ip_address_literal) {
  auto ctx = iocoro::io_context{};
  auto pool = iocoro::thread_pool{2};

  struct test_result {
    std::optional<std::error_code> error;
    std::size_t count = 0;
    std::string address;
    std::uint16_t port = 0;
  };

  auto result = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<test_result> {
    test_result r;
    auto resolver = iocoro::ip::tcp::resolver{pool.get_executor()};
    auto resolve_result = co_await resolver.async_resolve("127.0.0.1", "8080");

    if (!resolve_result) {
      r.error = resolve_result.error();
      co_return r;
    }

    r.count = resolve_result->size();
    if (!resolve_result->empty()) {
      auto const& ep = resolve_result->front();
      r.address = ep.address().to_string();
      r.port = ep.port();
    }

    co_return r;
  }());

  pool.stop();
  pool.join();

  if (result.error) {
    FAIL() << "Resolve failed: " << result.error->message();
  }

  EXPECT_EQ(result.count, 1u);
  EXPECT_EQ(result.address, "127.0.0.1");
  EXPECT_EQ(result.port, 8080);
}

TEST(resolver_test, resolve_ipv6_localhost) {
  auto ctx = iocoro::io_context{};
  auto pool = iocoro::thread_pool{2};

  struct test_result {
    std::optional<std::error_code> error;
    std::size_t count = 0;
    bool is_v6 = false;
    bool is_loopback = false;
    std::uint16_t port = 0;
  };

  auto result = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<test_result> {
    test_result r;
    auto resolver = iocoro::ip::tcp::resolver{pool.get_executor()};
    auto resolve_result = co_await resolver.async_resolve("::1", "443");

    if (!resolve_result) {
      r.error = resolve_result.error();
      co_return r;
    }

    r.count = resolve_result->size();
    if (!resolve_result->empty()) {
      auto const& ep = resolve_result->front();
      r.is_v6 = ep.address().is_v6();
      if (r.is_v6) {
        r.is_loopback = ep.address().to_v6().is_loopback();
      }
      r.port = ep.port();
    }

    co_return r;
  }());

  pool.stop();
  pool.join();

  if (result.error) {
    if (should_skip_network_test(*result.error)) {
      GTEST_SKIP() << "IPv6 unavailable: " << result.error->message();
    }
    FAIL() << "Resolve failed: " << result.error->message();
  }

  EXPECT_EQ(result.count, 1u);
  EXPECT_TRUE(result.is_v6);
  EXPECT_TRUE(result.is_loopback);
  EXPECT_EQ(result.port, 443);
}

TEST(resolver_test, resolve_service_name) {
  auto ctx = iocoro::io_context{};
  auto pool = iocoro::thread_pool{2};

  struct test_result {
    std::optional<std::error_code> error;
    std::size_t count = 0;
    std::uint16_t port = 0;
  };

  auto result = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<test_result> {
    test_result r;
    auto resolver = iocoro::ip::tcp::resolver{pool.get_executor()};
    auto resolve_result = co_await resolver.async_resolve("127.0.0.1", "http");

    if (!resolve_result) {
      r.error = resolve_result.error();
      co_return r;
    }

    r.count = resolve_result->size();
    if (!resolve_result->empty()) {
      r.port = resolve_result->front().port();
    }

    co_return r;
  }());

  pool.stop();
  pool.join();

  if (result.error) {
    FAIL() << "Resolve failed: " << result.error->message();
  }

  EXPECT_EQ(result.count, 1u);
  EXPECT_EQ(result.port, 80);
}

TEST(resolver_test, resolve_public_domain) {
  auto ctx = iocoro::io_context{};
  auto pool = iocoro::thread_pool{2};

  struct test_result {
    std::optional<std::error_code> error;
    std::size_t count = 0;
  };

  auto result = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<test_result> {
    test_result r;
    auto resolver = iocoro::ip::tcp::resolver{pool.get_executor()};
    auto resolve_result = co_await resolver.async_resolve("example.com", "80");

    if (!resolve_result) {
      r.error = resolve_result.error();
      co_return r;
    }

    r.count = resolve_result->size();

    // Validate that we got valid endpoints.
    for (auto const& ep : *resolve_result) {
      EXPECT_EQ(ep.port(), 80);
      EXPECT_FALSE(ep.address().to_string().empty());
    }

    co_return r;
  }());

  pool.stop();
  pool.join();

  if (result.error) {
    if (should_skip_network_test(*result.error)) {
      GTEST_SKIP() << "Network unavailable: " << result.error->message();
    }
    FAIL() << "Resolve failed: " << result.error->message();
  }

  EXPECT_GT(result.count, 0u);
}

TEST(resolver_test, multiple_resolves_sequentially) {
  auto ctx = iocoro::io_context{};
  auto pool = iocoro::thread_pool{2};

  int success_count = iocoro::sync_wait(ctx, [&]() -> iocoro::awaitable<int> {
    auto resolver = iocoro::ip::tcp::resolver{pool.get_executor()};
    int count = 0;

    // Resolve multiple hosts sequentially to test reusability.
    auto result1 = co_await resolver.async_resolve("127.0.0.1", "80");
    if (result1 && !result1->empty()) {
      ++count;
    }

    auto result2 = co_await resolver.async_resolve("::1", "443");
    if (result2 && !result2->empty()) {
      ++count;
    }

    auto result3 = co_await resolver.async_resolve("localhost", "8080");
    if (result3 && !result3->empty()) {
      ++count;
    }

    co_return count;
  }());

  pool.stop();
  pool.join();

  // At least the first resolve (127.0.0.1) should succeed.
  EXPECT_GE(success_count, 1);
}

}  // namespace
