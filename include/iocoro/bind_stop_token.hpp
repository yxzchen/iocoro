#pragma once

#include <iocoro/awaitable.hpp>

#include <stop_token>
#include <utility>

namespace iocoro {

/// Bind a parent stop token to an `awaitable`.
///
/// Semantics:
/// - Transfers ownership of `task`'s coroutine handle.
/// - Calls `inherit_stop_token(token)` on the coroutine promise.
/// - Returns a new `awaitable` that observes stop requests from `token`.
template <typename T>
auto bind_stop_token(std::stop_token token, awaitable<T> task) -> awaitable<T> {
  auto handle = task.release();
  handle.promise().inherit_stop_token(std::move(token));
  return awaitable<T>{handle};
}

}  // namespace iocoro

