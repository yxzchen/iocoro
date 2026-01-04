#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/cancellation_token.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/io/stream_concepts.hpp>
#include <iocoro/with_timeout.hpp>

#include <chrono>
#include <concepts>
#include <system_error>
#include <utility>

namespace iocoro::io {

/// Connect with a deadline.
///
/// Semantics:
/// - On success: returns {}.
/// - On timeout: returns error::timed_out.
/// - On external cancellation: returns error::operation_aborted.
///
/// Note: This requires the socket's async_connect to observe cancellation_token.
template <class Socket, class Endpoint, class Rep, class Period>
  requires async_connect_socket<Socket, Endpoint>
auto async_connect_timeout(Socket& s, Endpoint const& ep,
                           std::chrono::duration<Rep, Period> timeout)
  -> ::iocoro::awaitable<std::error_code> {
  co_return co_await ::iocoro::with_timeout(
    s.get_executor(),
    [&](::iocoro::cancellation_token tok) { return s.async_connect(ep, std::move(tok)); },
    timeout);
}

}  // namespace iocoro::io
