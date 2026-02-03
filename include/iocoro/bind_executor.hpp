#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/awaitable.hpp>

namespace iocoro {

/// Bind an executor to an `awaitable`.
///
/// Semantics:
/// - Transfers ownership of `task`'s coroutine handle.
/// - Sets the executor in the coroutine promise.
/// - Returns a new `awaitable` that resumes on `executor` when scheduled.
///
/// IMPORTANT: This does not start execution; it only changes where the coroutine is scheduled.
template <typename T>
[[nodiscard]] auto bind_executor(any_executor executor, awaitable<T> task) -> awaitable<T> {
  auto handle = task.release();
  handle.promise().set_executor(std::move(executor));
  return awaitable<T>{handle};
}

}  // namespace iocoro
