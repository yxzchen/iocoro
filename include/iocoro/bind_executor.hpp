#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/executor.hpp>

namespace iocoro {

/// Binds an executor to an awaitable, ensuring it executes on the specified executor.
///
/// This takes ownership of the awaitable's coroutine handle, sets the executor on its
/// promise, and returns a new awaitable wrapping the same coroutine.
///
/// @param executor The executor to bind to the awaitable
/// @param task The awaitable to bind
/// @return A new awaitable bound to the specified executor
template <typename T>
auto bind_executor(any_executor executor, awaitable<T> task) -> awaitable<T> {
  auto handle = task.release();
  handle.promise().set_executor(std::move(executor));
  return awaitable<T>{handle};
}

}  // namespace iocoro
