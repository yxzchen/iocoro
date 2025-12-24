#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/concepts.hpp>
#include <iocoro/io/with_timeout.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <system_error>

namespace iocoro::io {

/// Read at most `buf.size()` bytes into `buf`, but fail with `error::timed_out`
/// if the operation does not complete within `timeout`.
///
/// Notes:
/// - Requires `Stream::cancel()` so the pending I/O can be safely aborted on timeout.
/// - If the stream is cancelled externally (not by this timeout), `error::operation_aborted`
///   is propagated as-is.
template <detail::async_read_stream Stream, class Rep, class Period>
  requires detail::cancellable_stream<Stream>
auto async_read_some_timeout(Stream& s, std::span<std::byte> buf,
                             std::chrono::duration<Rep, Period> timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  return with_timeout_read(s, s.async_read_some(buf), timeout);
}

/// Composed operation: read exactly `buf.size()` bytes.
///
/// Notes:
/// - This is an algorithm layered on top of the Stream's `async_read_some` primitive.
/// - Concurrency rules (e.g. "only one read in-flight") are defined by the Stream type.
/// - If `async_read_some` yields 0 before the buffer is full, this returns `error::eof`.
template <detail::async_stream Stream>
auto async_read(Stream& s, std::span<std::byte> buf)
  -> awaitable<expected<std::size_t, std::error_code>> {
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
///
/// Notes:
/// - Requires `Stream::cancel()` so the pending I/O can be safely aborted on timeout.
/// - On timeout, this returns `error::timed_out` (not `error::operation_aborted`).
template <detail::async_stream Stream, class Rep, class Period>
  requires detail::cancellable_stream<Stream>
auto async_read_timeout(Stream& s, std::span<std::byte> buf,
                        std::chrono::duration<Rep, Period> timeout)
  -> awaitable<expected<std::size_t, std::error_code>> {
  return with_timeout_read(s, async_read(s, buf), timeout);
}

/// Reads until EOF or buffer is full.
///
/// Semantics:
/// - Repeatedly reads into `buf` until:
///   - async_read_some returns 0 (EOF), or
///   - `buf` is completely filled.
/// - Returns the number of bytes written to `buf`.
/// - If EOF is encountered after some bytes were read, returns success with that count.
/// - If an error occurs before any bytes are read, returns that error.
/// - If an error occurs after some bytes are read, returns that error (no partial success).
template <detail::async_stream Stream>
auto async_read_until_eof(Stream& s, std::span<std::byte> buf)
  -> awaitable<expected<std::size_t, std::error_code>> {
  std::size_t total = 0;

  while (!buf.empty()) {
    auto r = co_await s.async_read_some(buf);
    if (!r) {
      co_return r;
    }

    auto const n = *r;
    if (n == 0) {  // EOF
      break;
    }

    total += n;
    buf = buf.subspan(n);
  }

  co_return total;
}

}  // namespace iocoro::io
