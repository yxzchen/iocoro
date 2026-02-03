#include <gtest/gtest.h>

#include <iocoro/error.hpp>
#include <iocoro/io/read_until.hpp>
#include <iocoro/io_context.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace {

struct mock_read_stream {
  std::string data{};
  std::size_t pos{0};
  std::size_t max_chunk{(std::numeric_limits<std::size_t>::max)()};
  iocoro::any_io_executor ex{};

  auto get_executor() const noexcept -> iocoro::any_io_executor { return ex; }

  auto async_read_some(std::span<std::byte> buf) -> iocoro::awaitable<iocoro::result<std::size_t>> {
    if (pos >= data.size()) {
      co_return iocoro::result<std::size_t>(0);
    }

    auto const remaining = data.size() - pos;
    auto const n = std::min({buf.size(), max_chunk, remaining});
    std::memcpy(buf.data(), data.data() + pos, n);
    pos += n;
    co_return iocoro::result<std::size_t>(n);
  }
};

}  // namespace

TEST(async_read_until_test, finds_multibyte_delimiter_across_chunks_and_may_overread) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abc\r\nrest", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};
  std::array<std::byte, 1024> buf{};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    return iocoro::io::async_read_until(s, std::span{buf}, "\r\n");
  }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  auto const n = **r;
  ASSERT_EQ(n, 5U);
  auto const view = std::string_view{reinterpret_cast<char const*>(buf.data()), n};
  EXPECT_EQ(view, "abc\r\n");
}

TEST(async_read_until_test, completes_immediately_if_delimiter_already_present) {
  iocoro::io_context ctx;

  mock_read_stream s{
    .data = "SHOULD_NOT_BE_READ", .pos = 0, .max_chunk = 1, .ex = ctx.get_executor()};
  std::array<std::byte, 1024> buf{};

  std::string_view initial = "hello\n";
  std::memcpy(buf.data(), initial.data(), initial.size());

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    return iocoro::io::async_read_until(s, std::span{buf}, '\n', initial.size());
  }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 6U);
  EXPECT_EQ(s.pos, 0U);
}

TEST(async_read_until_test, returns_message_size_if_not_found_within_max_size) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abcdef", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};
  std::array<std::byte, 4> buf{};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    return iocoro::io::async_read_until(s, std::span{buf}, '\n');
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::message_size);
  auto const view = std::string_view{reinterpret_cast<char const*>(buf.data()), buf.size()};
  EXPECT_EQ(view, "abcd");
}

TEST(async_read_until_test, returns_eof_if_stream_ends_before_delimiter) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abc", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};
  std::array<std::byte, 1024> buf{};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    return iocoro::io::async_read_until(s, std::span{buf}, '\n');
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::eof);
}

TEST(async_read_until_test, empty_delimiter_returns_invalid_argument) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abc", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};
  std::array<std::byte, 4> buf{};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    return iocoro::io::async_read_until(s, std::span{buf}, std::string_view{});
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::invalid_argument);
}

TEST(async_read_until_test, invalid_initial_size_returns_invalid_argument) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abc", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};
  std::array<std::byte, 4> buf{};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    return iocoro::io::async_read_until(s, std::span{buf}, '\n', buf.size() + 1);
  }());

  ASSERT_TRUE(r);
  ASSERT_FALSE(*r);
  EXPECT_EQ(r->error(), iocoro::error::invalid_argument);
}

TEST(async_read_until_test, delimiter_at_buffer_end_is_detected) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abc\n", .pos = 0, .max_chunk = 2, .ex = ctx.get_executor()};
  std::array<std::byte, 4> buf{};

  auto r = iocoro::test::sync_wait(ctx, [&]() -> iocoro::awaitable<iocoro::result<std::size_t>> {
    return iocoro::io::async_read_until(s, std::span{buf}, '\n');
  }());

  ASSERT_TRUE(r);
  ASSERT_TRUE(*r);
  EXPECT_EQ(**r, 4U);
}
