#include <gtest/gtest.h>

#include <iocoro/co_spawn.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/async_read_until.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/src.hpp>

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
  auto ex = ctx.get_executor();

  mock_read_stream s{.data = "abc\r\nrest", .pos = 0, .max_chunk = 2};
  std::string out;

  std::error_code ec{};
  std::size_t n = 0;

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      auto r = co_await iocoro::io::async_read_until(s, out, "\r\n", 1024);
      if (!r) {
        ec = r.error();
        co_return;
      }
      n = *r;
    },
    iocoro::detached);

  (void)ctx.run();
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_EQ(n, 5U);
  ASSERT_GE(out.size(), n);
  EXPECT_EQ(out.substr(0, n), "abc\r\n");
}

TEST(async_read_until_test, completes_immediately_if_delimiter_already_present) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  mock_read_stream s{.data = "SHOULD_NOT_BE_READ", .pos = 0, .max_chunk = 1};
  std::string out = "hello\n";

  std::error_code ec{};
  std::size_t n = 0;

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      auto r = co_await iocoro::io::async_read_until(s, out, '\n', 1024);
      if (!r) {
        ec = r.error();
        co_return;
      }
      n = *r;
    },
    iocoro::detached);

  (void)ctx.run();
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(n, 6U);
  EXPECT_EQ(s.pos, 0U);
}

TEST(async_read_until_test, returns_message_size_if_not_found_within_max_size) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  mock_read_stream s{.data = "abcdef", .pos = 0, .max_chunk = 2};
  std::string out;

  std::error_code ec{};

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      auto r = co_await iocoro::io::async_read_until(s, out, '\n', 4);
      if (!r) ec = r.error();
    },
    iocoro::detached);

  (void)ctx.run();
  ASSERT_TRUE(ec);
  EXPECT_EQ(ec, iocoro::error::message_size);
  EXPECT_EQ(out, "abcd");
}

TEST(async_read_until_test, returns_eof_if_stream_ends_before_delimiter) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  mock_read_stream s{.data = "abc", .pos = 0, .max_chunk = 2};
  std::string out;

  std::error_code ec{};

  iocoro::co_spawn(
    ex,
    [&]() -> iocoro::awaitable<void> {
      auto r = co_await iocoro::io::async_read_until(s, out, '\n', 1024);
      if (!r) ec = r.error();
    },
    iocoro::detached);

  (void)ctx.run();
  ASSERT_TRUE(ec);
  EXPECT_EQ(ec, iocoro::error::eof);
  EXPECT_EQ(out, "abc");
}

}  // namespace
