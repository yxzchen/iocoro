#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/async_read_until.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/src.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>

namespace {

struct mock_read_stream {
  std::string data{};
  std::size_t pos{0};
  std::size_t max_chunk{(std::numeric_limits<std::size_t>::max)()};

  auto async_read_some(std::span<std::byte> buf)
    -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
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

TEST(async_read_until_test, finds_multibyte_delimiter_across_chunks_and_may_overread) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abc\r\nrest", .pos = 0, .max_chunk = 2};
  std::string out;

  auto r = iocoro::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_read_until(s, out, "\r\n", 1024);
    }());

  ASSERT_TRUE(r) << r.error().message();
  auto const n = *r;
  ASSERT_EQ(n, 5U);
  ASSERT_GE(out.size(), n);
  EXPECT_EQ(out.substr(0, n), "abc\r\n");
}

TEST(async_read_until_test, completes_immediately_if_delimiter_already_present) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "SHOULD_NOT_BE_READ", .pos = 0, .max_chunk = 1};
  std::string out = "hello\n";

  auto r = iocoro::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_read_until(s, out, '\n', 1024);
    }());

  ASSERT_TRUE(r) << r.error().message();
  auto const n = *r;
  EXPECT_EQ(n, 6U);
  EXPECT_EQ(s.pos, 0U);
}

TEST(async_read_until_test, returns_message_size_if_not_found_within_max_size) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abcdef", .pos = 0, .max_chunk = 2};
  std::string out;

  auto r = iocoro::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_read_until(s, out, '\n', 4);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::message_size);
  EXPECT_EQ(out, "abcd");
}

TEST(async_read_until_test, returns_eof_if_stream_ends_before_delimiter) {
  iocoro::io_context ctx;

  mock_read_stream s{.data = "abc", .pos = 0, .max_chunk = 2};
  std::string out;

  auto r = iocoro::sync_wait(
    ctx, [&]() -> iocoro::awaitable<iocoro::expected<std::size_t, std::error_code>> {
      return iocoro::io::async_read_until(s, out, '\n', 1024);
    }());

  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::eof);
  EXPECT_EQ(out, "abc");
}

}  // namespace
