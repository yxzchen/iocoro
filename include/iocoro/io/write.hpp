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

/// Composed operation: write exactly `buf.size()` bytes.
///
/// Notes:
/// - This is an algorithm layered on top of the Stream's `async_write_some` primitive.
/// - Concurrency rules (e.g. "only one write in-flight") are defined by the Stream type.
/// - If `async_write_some` yields 0 before the buffer is fully written, this returns
///   `error::broken_pipe`.
///
/// IMPORTANT - Buffer Lifetime:
/// The caller is responsible for ensuring the buffer remains valid until
/// the operation completes. If the operation is cancelled (via stop_token),
/// the buffer must still remain valid until the coroutine yields control.
/// Destroying the buffer while the operation is in progress results in
/// undefined behavior (use-after-free).
template <async_write_stream Stream>
[[nodiscard]] auto async_write(Stream& s, std::span<std::byte const> buf)
  -> awaitable<result<std::size_t>> {
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

template <async_write_stream Stream>
[[nodiscard]] auto async_write(Stream& s, net::const_buffer buf) -> awaitable<result<std::size_t>> {
  co_return co_await async_write(s, buf.as_span());
}

}  // namespace iocoro::io
