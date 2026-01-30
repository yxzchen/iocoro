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

/// Composed operation: read exactly `buf.size()` bytes.
///
/// Notes:
/// - This is an algorithm layered on top of the Stream's `async_read_some` primitive.
/// - Concurrency rules (e.g. "only one read in-flight") are defined by the Stream type.
/// - If `async_read_some` yields 0 before the buffer is full, this returns `error::eof`.
template <async_read_stream Stream>
auto async_read(Stream& s, std::span<std::byte> buf)
  -> awaitable<expected<std::size_t, std::error_code>> {
  auto tok = co_await this_coro::cancellation_token;
  auto const wanted = buf.size();
  std::size_t total = 0;

  while (!buf.empty()) {
    auto r = co_await s.async_read_some(buf);
    if (!r) {
      co_return r;
    }

    auto const n = *r;
    if (n == 0) {  // EOF
      co_return unexpected(error::eof);
    }

    total += n;
    buf = buf.subspan(n);
  }

  co_return wanted;
}

/// Composed operation: read exactly `buf.size()` bytes, but fail with `error::timed_out`
/// if the overall operation does not complete within `timeout`.
template <async_read_stream Stream, class Rep, class Period>
auto async_read_timeout(Stream& s, std::span<std::byte> buf,
                        std::chrono::duration<Rep, Period> timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  co_await this_coro::switch_to(s.get_executor());
  auto scope = co_await this_coro::scoped_timeout(timeout);
  auto r = co_await async_read(s, buf);
  if (!r && r.error() == error::operation_aborted && scope.timed_out()) {
    co_return unexpected(error::timed_out);
  }
  co_return r;
}

/// Read at most `buf.size()` bytes into `buf`, but fail with `error::timed_out`
/// if the operation does not complete within `timeout`.
template <async_read_stream Stream, class Rep, class Period>
auto async_read_some_timeout(Stream& s, std::span<std::byte> buf,
                             std::chrono::duration<Rep, Period> timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  co_await this_coro::switch_to(s.get_executor());
  auto scope = co_await this_coro::scoped_timeout(timeout);
  auto r = co_await s.async_read_some(buf);
  if (!r && r.error() == error::operation_aborted && scope.timed_out()) {
    co_return unexpected(error::timed_out);
  }
  co_return r;
}

}  // namespace iocoro::io
