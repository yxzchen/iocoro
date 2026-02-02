#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <iocoro/io/stream_concepts.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>

namespace iocoro::io {

namespace detail {
/// Search for `delim` in the byte span.
/// Returns the position of the first occurrence, or `npos` if not found.
inline auto find_in_span(std::span<std::byte const> data, std::span<std::byte const> delim)
  -> std::size_t {
  constexpr auto npos = static_cast<std::size_t>(-1);

  if (delim.empty() || data.size() < delim.size()) {
    return npos;
  }

  auto it = std::search(data.begin(), data.end(), delim.begin(), delim.end());
  if (it == data.end()) {
    return npos;
  }

  return static_cast<std::size_t>(std::distance(data.begin(), it));
}
}  // namespace detail

/// Composed operation: read from a stream into a buffer until `delim` is found.
///
/// Semantics:
/// - Reads bytes into `buf` starting at offset `initial_size`.
/// - Returns the total number of bytes in `buf` up to and including the first occurrence of `delim`.
/// - If `buf[0..initial_size)` already contains `delim`, completes immediately without reading.
/// - If EOF is reached before `delim` is found, returns `error::eof`.
/// - If the buffer would be filled (buf.size() bytes) without finding `delim`, returns `error::message_size`.
///
/// Note: the underlying `async_read_some` may read past the delimiter; in that case, `buf`
/// will contain extra bytes after the returned count.
template <async_read_stream Stream>
auto async_read_until(Stream& s, std::span<std::byte> buf, std::span<std::byte const> delim,
                      std::size_t initial_size = 0)
  -> awaitable<result<std::size_t>> {
  if (delim.empty()) {
    co_return unexpected(error::invalid_argument);
  }

  if (initial_size > buf.size()) {
    co_return unexpected(error::invalid_argument);
  }

  auto const max_size = buf.size();
  auto current_size = initial_size;

  // Fast path: delimiter already present in initial data.
  if (current_size >= delim.size()) {
    auto const pos = detail::find_in_span(buf.subspan(0, current_size), delim);
    if (pos != static_cast<std::size_t>(-1)) {
      co_return pos + delim.size();
    }
  }

  // Only the last (delim.size() - 1) bytes can form a delimiter crossing a boundary.
  auto search_from = current_size > (delim.size() - 1) ? current_size - (delim.size() - 1) : 0U;

  while (current_size < max_size) {
    auto const remaining = max_size - current_size;
    auto read_buf = buf.subspan(current_size, remaining);

    auto r = co_await s.async_read_some(read_buf);
    if (!r) {
      co_return r;
    }

    auto const n = *r;
    if (n == 0) {  // EOF
      co_return unexpected(error::eof);
    }

    current_size += n;

    // Search only the newly affected suffix.
    auto const pos = detail::find_in_span(buf.subspan(0, current_size).subspan(search_from), delim);
    if (pos != static_cast<std::size_t>(-1)) {
      co_return search_from + pos + delim.size();
    }

    // Next search only needs to start where a delimiter could overlap the boundary.
    search_from = current_size > (delim.size() - 1) ? current_size - (delim.size() - 1) : 0U;
  }

  co_return unexpected(error::message_size);
}

/// Convenience overload accepting string_view delimiter.
template <async_read_stream Stream>
auto async_read_until(Stream& s, std::span<std::byte> buf, std::string_view delim,
                      std::size_t initial_size = 0)
  -> awaitable<result<std::size_t>> {
  auto const delim_bytes = std::span<std::byte const>{
    reinterpret_cast<std::byte const*>(delim.data()), delim.size()
  };
  co_return co_await async_read_until(s, buf, delim_bytes, initial_size);
}

/// Convenience overload for single-character delimiters.
template <async_read_stream Stream>
auto async_read_until(Stream& s, std::span<std::byte> buf, char delim,
                      std::size_t initial_size = 0)
  -> awaitable<result<std::size_t>> {
  std::byte const d[1] = {static_cast<std::byte>(delim)};
  co_return co_await async_read_until(s, buf, std::span<std::byte const>{d, 1}, initial_size);
}

}  // namespace iocoro::io
