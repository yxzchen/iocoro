#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/expected.hpp>

#include <cstddef>
#include <concepts>
#include <span>
#include <system_error>

namespace iocoro::io {

/// Minimal async stream concepts used by composed I/O algorithms.
///
/// IMPORTANT: concepts cannot enforce semantics. The following contracts are normative.
///
/// async_read_some contract:
/// - On success, returns `n > 0` bytes read.
/// - Returning `n == 0` indicates EOF (orderly shutdown by peer).
///
/// async_write_some contract:
/// - On success, returns `n > 0` bytes written.
/// - Returning `n == 0` is considered a fatal condition by composed algorithms
///   (typically treated as `iocoro::error::broken_pipe`).
///
/// Error codes:
/// - Prefer `iocoro::error` values where applicable (e.g. `eof`, `broken_pipe`,
///   `operation_aborted`, `not_open`, `busy`, ...).
template <class Stream>
concept async_read_stream =
  requires(Stream& s, std::span<std::byte> rbuf) {
    requires std::same_as<decltype(s.async_read_some(rbuf)),
                          awaitable<expected<std::size_t, std::error_code>>>;
  };

template <class Stream>
concept async_write_stream =
  requires(Stream& s, std::span<std::byte const> wbuf) {
    requires std::same_as<decltype(s.async_write_some(wbuf)),
                          awaitable<expected<std::size_t, std::error_code>>>;
  };

template <class Stream>
concept async_stream = async_read_stream<Stream> && async_write_stream<Stream>;

}  // namespace iocoro::io


