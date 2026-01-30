#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/cancellation_token.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/with_timeout.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <system_error>

namespace iocoro::io {

/// Composed operation: write exactly `buf.size()` bytes.
///
/// Notes:
/// - This is an algorithm layered on top of the Stream's `async_write_some` primitive.
/// - Concurrency rules (e.g. "only one write in-flight") are defined by the Stream type.
/// - If `async_write_some` yields 0 before the buffer is fully written, this returns
///   `error::broken_pipe`.
template <async_write_stream Stream>
auto async_write(Stream& s, std::span<std::byte const> buf)
  -> awaitable<expected<std::size_t, std::error_code>> {
  auto tok = co_await this_coro::cancellation_token;
  auto const wanted = buf.size();

  while (!buf.empty()) {
    auto r = co_await s.async_write_some(buf);
    if (!r) {
      co_return r;
    }

    auto const n = *r;
    if (n == 0) {
      co_return unexpected(error::broken_pipe);
    }

    buf = buf.subspan(n);
  }

  co_return wanted;
}

/// Composed operation: write exactly `buf.size()` bytes, but fail with `error::timed_out`
/// if the overall operation does not complete within `timeout`.
template <async_write_stream Stream, class Rep, class Period>
auto async_write_timeout(Stream& s, std::span<std::byte const> buf,
                         std::chrono::duration<Rep, Period> timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  co_await this_coro::switch_to(s.get_executor());
  auto scope = co_await this_coro::scoped_timeout(timeout);
  auto r = co_await async_write(s, buf);
  if (!r && r.error() == error::operation_aborted && scope.timed_out()) {
    co_return unexpected(error::timed_out);
  }
  co_return r;
}

/// Write at most `buf.size()` bytes from `buf`, but fail with `error::timed_out`
/// if the operation does not complete within `timeout`.
template <async_write_stream Stream, class Rep, class Period>
auto async_write_some_timeout(Stream& s, std::span<std::byte const> buf,
                              std::chrono::duration<Rep, Period> timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  co_await this_coro::switch_to(s.get_executor());
  auto scope = co_await this_coro::scoped_timeout(timeout);
  auto r = co_await s.async_write_some(buf);
  if (!r && r.error() == error::operation_aborted && scope.timed_out()) {
    co_return unexpected(error::timed_out);
  }
  co_return r;
}

}  // namespace iocoro::io
