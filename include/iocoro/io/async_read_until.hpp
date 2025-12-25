#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/error.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/stream_concepts.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace iocoro::io {

/// Composed operation: read from a stream into a dynamic string buffer until `delim` is found.
///
/// Semantics:
/// - Appends received bytes to `out`.
/// - Returns the number of bytes in `out` up to and including the first occurrence of `delim`.
/// - If `out` already contains `delim`, completes immediately without reading.
/// - If EOF is reached before `delim` is found, returns `error::eof`.
/// - If `out` would grow beyond `max_size` without finding `delim`, returns
/// `error::message_size`.
///
/// Note: the underlying `async_read_some` may read past the delimiter; in that case, `out`
/// will contain extra bytes after the returned count.
template <async_read_stream Stream>
auto async_read_until(Stream& s, std::string& out, std::string_view delim,
                      std::size_t max_size = 64 * 1024)
  -> awaitable<expected<std::size_t, std::error_code>> {
  if (delim.empty()) {
    co_return unexpected(error::invalid_argument);
  }

  // Fast path: delimiter already present.
  if (auto const pos = out.find(delim); pos != std::string::npos) {
    co_return pos + delim.size();
  }

  // Only the last (delim.size() - 1) bytes can form a delimiter crossing a boundary.
  auto search_from = out.size() > (delim.size() - 1) ? out.size() - (delim.size() - 1) : 0U;

  constexpr std::size_t read_chunk = 1024;
  std::array<std::byte, read_chunk> tmp{};

  while (out.size() < max_size) {
    auto const cap = max_size - out.size();
    auto const to_read = std::min<std::size_t>(tmp.size(), cap);
    if (to_read == 0) {
      break;
    }

    auto r = co_await s.async_read_some(std::span<std::byte>(tmp.data(), to_read));
    if (!r) {
      co_return r;
    }

    auto const n = *r;
    if (n == 0) {  // EOF
      co_return unexpected(error::eof);
    }

    out.append(reinterpret_cast<char const*>(tmp.data()), n);

    // Search only the newly affected suffix.
    auto const pos = out.find(delim, search_from);
    if (pos != std::string::npos) {
      co_return pos + delim.size();
    }

    // Next search only needs to start where a delimiter could overlap the boundary.
    auto const new_size = out.size();
    search_from = new_size > (delim.size() - 1) ? new_size - (delim.size() - 1) : 0U;
  }

  co_return unexpected(error::message_size);
}

/// Convenience overload for single-character delimiters.
template <async_read_stream Stream>
auto async_read_until(Stream& s, std::string& out, char delim, std::size_t max_size = 64 * 1024)
  -> awaitable<expected<std::size_t, std::error_code>> {
  char const d[1] = {delim};
  co_return co_await async_read_until(s, out, std::string_view{d, 1}, max_size);
}

}  // namespace iocoro::io
