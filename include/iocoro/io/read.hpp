#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <iocoro/io/stream_concepts.hpp>

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
  -> awaitable<result<std::size_t>> {
  auto const wanted = buf.size();

  while (!buf.empty()) {
    auto r = co_await s.async_read_some(buf);
    if (!r) {
      co_return r;
    }

    auto const n = *r;
    if (n == 0) {  // EOF
      co_return unexpected(error::eof);
    }

    buf = buf.subspan(n);
  }

  co_return wanted;
}

}  // namespace iocoro::io
