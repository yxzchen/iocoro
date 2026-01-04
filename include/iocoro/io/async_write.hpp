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
auto async_write(Stream& s, std::span<std::byte const> buf, cancellation_token tok = {})
  -> awaitable<expected<std::size_t, std::error_code>> {
  auto const wanted = buf.size();

  while (!buf.empty()) {
    auto r = co_await s.async_write_some(buf, tok);
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
///
/// Notes:
/// - Requires `Stream::cancel()` so the pending I/O can be safely aborted on timeout.
/// - On timeout, this returns `error::timed_out` (not `error::operation_aborted`).
template <async_write_stream Stream, class Rep, class Period>
auto async_write_timeout(Stream& s, std::span<std::byte const> buf,
                         std::chrono::duration<Rep, Period> timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  co_return co_await with_timeout(
    s.get_executor(),
    [&](cancellation_token tok) {
      return async_write(s, buf, std::move(tok));
    },
    timeout);
}

/// Write at most `buf.size()` bytes from `buf`, but fail with `error::timed_out`
/// if the operation does not complete within `timeout`.
///
/// Notes:
/// - Requires `Stream::cancel()` so the pending I/O can be safely aborted on timeout.
/// - If the stream is cancelled externally (not by this timeout), `error::operation_aborted`
///   is propagated as-is.
template <async_write_stream Stream, class Rep, class Period>
auto async_write_some_timeout(Stream& s, std::span<std::byte const> buf,
                              std::chrono::duration<Rep, Period> timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  co_return co_await with_timeout(
    s.get_executor(),
    [&](cancellation_token tok) {
      return s.async_write_some(buf, std::move(tok));
    },
    timeout);
}

}  // namespace iocoro::io
