#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/concepts.hpp>

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
template <async_stream Stream>
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
      co_return unexpected<std::error_code>(error::eof);
    }

    total += n;
    buf = buf.subspan(n);
  }

  co_return expected<std::size_t, std::error_code>(wanted);
}

/// Composed operation: read until EOF or the buffer is full.
///
/// - Returns the number of bytes read.
/// - EOF (read_some yielding 0) is treated as a successful completion.
template <async_stream Stream>
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

  co_return expected<std::size_t, std::error_code>(total);
}

}  // namespace iocoro::io


