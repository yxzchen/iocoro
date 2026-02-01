#include <gtest/gtest.h>

#include <iocoro/error.hpp>
#include <iocoro/io/read.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>

namespace {

struct mock_read_stream {
  std::string data{};
  std::size_t pos{0};
  std::size_t max_chunk{1024};
  iocoro::any_io_executor ex{};
  std::error_code next_error{};
  std::size_t error_after{(std::numeric_limits<std::size_t>::max)()};
  std::error_code error_after_code{};

  auto get_executor() const noexcept -> iocoro::any_io_executor { return ex; }

  auto async_read_some(std::span<std::byte> buf)
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
    if (next_error) {
      auto ec = next_error;
      next_error = {};
      co_return iocoro::unexpected(ec);
    }
    if (pos >= error_after && error_after_code) {
      auto ec = error_after_code;
      error_after = (std::numeric_limits<std::size_t>::max)();
      error_after_code = {};
      co_return iocoro::unexpected(ec);
    }

    if (pos >= data.size()) {
      co_return iocoro::expected<std::size_t, std::error_code>(0);
    }

    auto const remaining = data.size() - pos;
    auto const n = std::min({buf.size(), max_chunk, remaining});
    std::memcpy(buf.data(), data.data() + pos, n);
    pos += n;
    co_return iocoro::expected<std::size_t, std::error_code>(n);
  }
};

}  // namespace

TEST(async_read_test, reads_exactly_full_buffer) {
  iocoro::io_context ctx;
  mock_read_stream s{.data = "abcdef", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};

  std::array<std::byte, 6> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await iocoro::io::async_read(s, std::span{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, buf.size());
  auto view = std::string_view{reinterpret_cast<char const*>(buf.data()), buf.size()};
  EXPECT_EQ(view, "abcdef");
}

TEST(async_read_test, returns_eof_if_stream_ends_before_full) {
  iocoro::io_context ctx;
  mock_read_stream s{.data = "abc", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};

  std::array<std::byte, 6> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await iocoro::io::async_read(s, std::span{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::eof);
}

TEST(async_read_test, propagates_errors_from_read_some) {
  iocoro::io_context ctx;
  mock_read_stream s{.data = "abc", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};
  s.next_error = std::make_error_code(std::errc::bad_file_descriptor);

  std::array<std::byte, 4> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      co_return co_await iocoro::io::async_read(s, std::span{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), std::make_error_code(std::errc::bad_file_descriptor));
}

TEST(async_read_test, empty_buffer_returns_zero_without_reading) {
  iocoro::io_context ctx;
  mock_read_stream s{.data = "abc", .pos = 0, .max_chunk = 1, .ex = ctx.get_executor()};

  std::array<std::byte, 1> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_read(s, std::span<std::byte>{buf}.first(0));
    }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 0U);
  EXPECT_EQ(s.pos, 0U);
}

TEST(async_read_test, error_after_partial_progress_is_propagated) {
  iocoro::io_context ctx;
  mock_read_stream s{.data = "abcd", .pos = 0, .max_chunk = 1, .ex = ctx.get_executor()};
  s.error_after = 2;
  s.error_after_code = std::make_error_code(std::errc::io_error);

  std::array<std::byte, 4> buf{};
  auto r = iocoro::test::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_read(s, std::span{buf});
    }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), std::make_error_code(std::errc::io_error));
  EXPECT_EQ(s.pos, 2U);
}
