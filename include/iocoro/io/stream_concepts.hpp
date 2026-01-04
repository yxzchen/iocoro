#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/cancellation_token.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_executor.hpp>

#include <concepts>
#include <cstddef>
#include <span>
#include <system_error>

namespace iocoro::io {

/// Minimal asynchronous stream concept for composed I/O algorithms.
///
/// This concept models a *stream-oriented transport*, such as TCP or
/// other byte-stream based abstractions. It is intentionally minimal
/// and is designed to support higher-level composed operations like
/// `async_read` / `async_write`.
///
/// A type `Stream` models `async_stream` if it satisfies the following
/// requirements:
///
/// 1. It provides an asynchronous read primitive:
///    ```
///    async_read_some(std::span<std::byte>, cancellation_token)
///      -> awaitable<expected<std::size_t, std::error_code>>
///    ```
///
/// 2. It provides an asynchronous write primitive:
///    ```
///    async_write_some(std::span<std::byte const>, cancellation_token)
///      -> awaitable<expected<std::size_t, std::error_code>>
///    ```
///
/// Semantics and conventions:
///
/// - The returned `std::size_t` indicates the number of bytes transferred.
/// - A return value of `0` has *stream semantics*:
///   - For reads, it indicates end-of-stream (EOF).
///   - For writes, it indicates that no further progress can be made
///     (e.g. peer closed, broken pipe).
///
/// - Errors are reported via `expected<..., std::error_code>`.
///   Transport-level failures (I/O errors, connection reset, etc.)
///   must be represented as a non-empty `std::error_code`.
///
/// - This concept is intended for *byte-stream transports* only.
///   It is **not** suitable for message-oriented or record-oriented
///   abstractions (e.g. UDP, datagram sockets, framed protocols),
///   where partial reads/writes or zero-length transfers may have
///   different meanings.

template <class Stream>
concept io_executor_stream = requires(Stream& s) {
  { s.get_executor() } -> std::same_as<io_executor>;
};

template <class Stream>
concept async_read_stream =
  io_executor_stream<Stream> && requires(Stream& s, std::span<std::byte> rbuf, cancellation_token tok) {
    requires std::same_as<decltype(s.async_read_some(rbuf, tok)),
                          awaitable<expected<std::size_t, std::error_code>>>;
  };

template <class Stream>
concept async_write_stream =
  io_executor_stream<Stream> &&
  requires(Stream& s, std::span<std::byte const> wbuf, cancellation_token tok) {
    requires std::same_as<decltype(s.async_write_some(wbuf, tok)),
                          awaitable<expected<std::size_t, std::error_code>>>;
  };

template <class Stream>
concept async_stream = async_read_stream<Stream> && async_write_stream<Stream>;

}  // namespace iocoro::io
