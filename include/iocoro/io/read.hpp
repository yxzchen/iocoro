#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/net/buffer.hpp>
#include <iocoro/result.hpp>

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
///
/// IMPORTANT - Buffer Lifetime:
/// The caller is responsible for ensuring the buffer remains valid until
/// the operation completes. If the operation is cancelled (via stop_token),
/// the buffer must still remain valid until the coroutine yields control.
/// Destroying the buffer while the operation is in progress results in
/// undefined behavior (use-after-free).
template <async_read_stream Stream>
[[nodiscard]] auto async_read(Stream& s,
                              std::span<std::byte> buf) -> awaitable<result<std::size_t>> {
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

template <async_read_stream Stream>
[[nodiscard]] auto async_read(Stream& s,
                              net::mutable_buffer buf) -> awaitable<result<std::size_t>> {
  return async_read(s, buf.as_span());
}

}  // namespace iocoro::io
