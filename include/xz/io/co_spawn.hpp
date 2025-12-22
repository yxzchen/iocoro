#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/detached.hpp>
#include <xz/io/detail/executor_guard.hpp>
#include <xz/io/expected.hpp>
#include <xz/io/executor.hpp>
#include <xz/io/use_awaitable.hpp>

#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>

namespace xz::io {

namespace detail {

struct on_executor_awaiter {
  executor ex;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) const {
    ex.post([h, ex = ex]() mutable {
      detail::executor_guard g{ex};
      h.resume();
    });
  }

  void await_resume() const noexcept {}
};

inline auto on_executor(executor ex) noexcept -> on_executor_awaiter { return on_executor_awaiter{ex}; }

template <typename T>
using spawn_expected = ::xz::io::expected<T, std::exception_ptr>;

template <typename F, typename T>
concept completion_callback_for =
  std::invocable<F&, spawn_expected<T>> && (!std::same_as<std::remove_cvref_t<F>, detached_t>) &&
  (!std::same_as<std::remove_cvref_t<F>, use_awaitable_t>);

template <typename T>
auto bind_executor(executor ex, awaitable<T> a) -> awaitable<T> {
  auto h = a.release();
  h.promise().set_executor(ex);
  return awaitable<T>{h};
}

template <typename T, typename Completion>
auto completion_wrapper(executor ex, awaitable<T> a, Completion completion) -> awaitable<void> {
  // Ensure the task itself is bound to `ex` before it starts.
  auto bound = bind_executor<T>(ex, std::move(a));

  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(bound);
      try {
        completion(spawn_expected<void>{});
      } catch (...) {
        // Completion callback exceptions are swallowed (detached semantics).
      }
    } else {
      auto v = co_await std::move(bound);
      try {
        completion(spawn_expected<T>{std::move(v)});
      } catch (...) {
        // Completion callback exceptions are swallowed (detached semantics).
      }
    }
  } catch (...) {
    auto ep = std::current_exception();
    try {
      completion(spawn_expected<T>{unexpected<std::exception_ptr>(ep)});
    } catch (...) {
      // Completion callback exceptions are swallowed (detached semantics).
    }
  }
}

}  // namespace detail

/// Start an awaitable on the given executor (detached / fire-and-forget).
///
/// Ownership of the coroutine frame is detached from the passed awaitable:
/// the frame will be destroyed at final_suspend.
template <typename T>
void co_spawn(executor ex, awaitable<T> a, detached_t) {
  auto h = a.release();

  h.promise().set_executor(ex);
  h.promise().detach();

  ex.post([h, ex]() mutable {
    detail::executor_guard g{ex};
    try {
      h.resume();
    } catch (...) {
      // Detached mode: swallow exceptions
    }
  });
}

/// Detached overload (default).
template <typename T>
void co_spawn(executor ex, awaitable<T> a) {
  co_spawn(ex, std::move(a), detached);
}

/// Start an awaitable on the given executor, returning an awaitable that can be awaited
/// to obtain the result (exception is rethrown on await_resume()).
template <typename T>
auto co_spawn(executor ex, awaitable<T> a, use_awaitable_t) -> awaitable<T> {
  // Bind the spawned coroutine to the target executor before first resume so
  // `co_await this_coro::executor` works from the start of the task.
  auto bound = detail::bind_executor<T>(ex, std::move(a));

  if constexpr (std::is_void_v<T>) {
    co_await detail::on_executor(ex);
    co_await std::move(bound);
    co_return;
  } else {
    co_await detail::on_executor(ex);
    co_return co_await std::move(bound);
  }
}

/// Start an awaitable on the given executor, invoking a completion callback with either
/// the result or an exception.
template <typename T, typename F>
  requires detail::completion_callback_for<F, T>
void co_spawn(executor ex, awaitable<T> a, F&& completion) {
  using completion_t = std::remove_cvref_t<F>;
  co_spawn(ex,
           detail::completion_wrapper<T, completion_t>(ex, std::move(a),
                                                      completion_t(std::forward<F>(completion))),
           detached);
}

}  // namespace xz::io
