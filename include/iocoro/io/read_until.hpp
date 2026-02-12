#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/net/buffer.hpp>
#include <iocoro/result.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <span>
#include <string_view>
#include <system_error>

namespace iocoro::io {

namespace detail {
[[nodiscard]] inline auto find_byte_in_span(std::span<std::byte const> data, std::byte needle)
  -> std::size_t {
  constexpr auto npos = static_cast<std::size_t>(-1);
  if (data.empty()) {
    return npos;
  }

  auto const* begin = static_cast<void const*>(data.data());
  auto const value = static_cast<unsigned char>(needle);
  auto const* found = std::memchr(begin, value, data.size());
  if (!found) {
    return npos;
  }
  auto const* data_ptr = reinterpret_cast<unsigned char const*>(data.data());
  auto const* found_ptr = reinterpret_cast<unsigned char const*>(found);
  return static_cast<std::size_t>(found_ptr - data_ptr);
}

/// Search for `delim` in the byte span.
/// Returns the position of the first occurrence, or `npos` if not found.
[[nodiscard]] inline auto find_in_span(std::span<std::byte const> data,
                                       std::span<std::byte const> delim) -> std::size_t {
  constexpr auto npos = static_cast<std::size_t>(-1);

  if (delim.empty() || data.size() < delim.size()) {
    return npos;
  }

  if (delim.size() == 1) {
    return find_byte_in_span(data, delim[0]);
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
///
/// IMPORTANT - Buffer Lifetime:
/// The caller is responsible for ensuring the buffer remains valid until
/// the operation completes. If the operation is cancelled (via stop_token),
/// the buffer must still remain valid until the coroutine yields control.
/// Destroying the buffer while the operation is in progress results in
/// undefined behavior (use-after-free).
template <async_read_stream Stream>
[[nodiscard]] auto async_read_until(Stream& s, std::span<std::byte> buf,
                                    std::span<std::byte const> delim, std::size_t initial_size = 0)
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

template <async_read_stream Stream>
[[nodiscard]] auto async_read_until(Stream& s, net::mutable_buffer buf, net::const_buffer delim,
                                    std::size_t initial_size = 0)
  -> awaitable<result<std::size_t>> {
  return async_read_until(s, buf.as_span(), delim.as_span(), initial_size);
}

/// Convenience overload accepting string_view delimiter.
template <async_read_stream Stream>
[[nodiscard]] auto async_read_until(Stream& s, net::mutable_buffer buf, std::string_view delim,
                                    std::size_t initial_size = 0)
  -> awaitable<result<std::size_t>> {
  return async_read_until(s, buf.as_span(), delim, initial_size);
}

/// Convenience overload accepting string_view delimiter.
template <async_read_stream Stream>
[[nodiscard]] auto async_read_until(Stream& s, std::span<std::byte> buf, std::string_view delim,
                                    std::size_t initial_size = 0)
  -> awaitable<result<std::size_t>> {
  return async_read_until(s, buf, net::buffer(delim).as_span(), initial_size);
}

/// Convenience overload for single-character delimiters.
template <async_read_stream Stream>
[[nodiscard]] auto async_read_until(Stream& s, std::span<std::byte> buf, char delim,
                                    std::size_t initial_size = 0)
  -> awaitable<result<std::size_t>> {
  if (initial_size > buf.size()) {
    co_return unexpected(error::invalid_argument);
  }

  auto const max_size = buf.size();
  auto current_size = initial_size;
  auto const delim_byte = static_cast<std::byte>(delim);

  if (current_size > 0) {
    auto const pos = detail::find_byte_in_span(buf.subspan(0, current_size), delim_byte);
    if (pos != static_cast<std::size_t>(-1)) {
      co_return pos + 1;
    }
  }

  auto search_from = current_size;
  while (current_size < max_size) {
    auto r = co_await s.async_read_some(buf.subspan(current_size));
    if (!r) {
      co_return r;
    }

    auto const n = *r;
    if (n == 0) {
      co_return unexpected(error::eof);
    }

    current_size += n;
    auto const pos =
      detail::find_byte_in_span(buf.subspan(0, current_size).subspan(search_from), delim_byte);
    if (pos != static_cast<std::size_t>(-1)) {
      co_return search_from + pos + 1;
    }
    search_from = current_size;
  }

  co_return unexpected(error::message_size);
}

/// Convenience overload for single-character delimiters.
template <async_read_stream Stream>
[[nodiscard]] auto async_read_until(Stream& s, net::mutable_buffer buf, char delim,
                                    std::size_t initial_size = 0)
  -> awaitable<result<std::size_t>> {
  return async_read_until(s, buf.as_span(), delim, initial_size);
}

}  // namespace iocoro::io
