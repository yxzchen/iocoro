#pragma once

#include <iocoro/awaitable.hpp>
#include <stop_token>
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
/// Note: This requires the socket's async_connect to observe stop_token.
template <class Socket, class Endpoint, class Rep, class Period>
  requires async_connect_socket<Socket, Endpoint>
auto async_connect_timeout(Socket& s, Endpoint const& ep,
                           std::chrono::duration<Rep, Period> timeout)
  -> ::iocoro::awaitable<std::error_code> {
  co_await ::iocoro::this_coro::switch_to(s.get_executor());
  auto scope = co_await ::iocoro::this_coro::scoped_timeout(timeout);
  auto ec = co_await s.async_connect(ep);
  if (ec == ::iocoro::error::operation_aborted && scope.timed_out()) {
    co_return ::iocoro::error::timed_out;
  }
  co_return ec;
}

}  // namespace iocoro::io
