#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/concepts.hpp>

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
template <async_stream Stream>
auto async_write(Stream& s, std::span<std::byte const> buf)
  -> awaitable<expected<std::size_t, std::error_code>> {
  auto const wanted = buf.size();
  std::size_t total = 0;

  while (!buf.empty()) {
    auto r = co_await s.async_write_some(buf);
    if (!r) {
      co_return r;
    }

    auto const n = *r;
    if (n == 0) {
      co_return unexpected<std::error_code>(error::broken_pipe);
    }

    total += n;
    buf = buf.subspan(n);
  }

  (void)total;
  co_return expected<std::size_t, std::error_code>(wanted);
}

}  // namespace iocoro::io


