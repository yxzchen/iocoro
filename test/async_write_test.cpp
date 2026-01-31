#include <gtest/gtest.h>

#include <iocoro/error.hpp>
#include <iocoro/io/write.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

namespace {

struct mock_write_stream {
  std::string data{};
  std::size_t max_chunk{1024};
  iocoro::any_io_executor ex{};
  std::error_code next_error{};
  bool return_zero{false};

  auto get_executor() const noexcept -> iocoro::any_io_executor { return ex; }

  auto async_write_some(std::span<std::byte const> buf)
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
    if (next_error) {
      auto ec = next_error;
      next_error = {};
      co_return iocoro::unexpected(ec);
    }
    if (return_zero) {
      co_return iocoro::expected<std::size_t, std::error_code>(0);
    }
    auto const n = std::min(buf.size(), max_chunk);
    data.append(reinterpret_cast<char const*>(buf.data()), n);
    co_return iocoro::expected<std::size_t, std::error_code>(n);
  }
};

}  // namespace

TEST(async_write_test, writes_entire_buffer) {
  iocoro::io_context ctx;
  mock_write_stream s{.data = {}, .max_chunk = 2, .ex = ctx.get_executor()};

  std::array<std::byte, 6> buf{};
  std::memcpy(buf.data(), "abcdef", buf.size());

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await iocoro::io::async_write(s, std::span<std::byte const>{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, buf.size());
  EXPECT_EQ(s.data, "abcdef");
}

TEST(async_write_test, returns_broken_pipe_on_zero_progress) {
  iocoro::io_context ctx;
  mock_write_stream s{.data = {}, .max_chunk = 2, .ex = ctx.get_executor(), .return_zero = true};

  std::array<std::byte, 2> buf{};
  std::memcpy(buf.data(), "ab", buf.size());

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await iocoro::io::async_write(s, std::span<std::byte const>{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::broken_pipe);
}

TEST(async_write_test, propagates_errors_from_write_some) {
  iocoro::io_context ctx;
  mock_write_stream s{.data = {}, .max_chunk = 2, .ex = ctx.get_executor()};
  s.next_error = std::make_error_code(std::errc::broken_pipe);

  std::array<std::byte, 2> buf{};
  std::memcpy(buf.data(), "ab", buf.size());

  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await iocoro::io::async_write(s, std::span<std::byte const>{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), std::make_error_code(std::errc::broken_pipe));
}
