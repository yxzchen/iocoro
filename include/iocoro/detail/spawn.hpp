#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/expected.hpp>

#include <atomic>
#include <concepts>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace iocoro::detail {

template <typename T>
using spawn_expected = expected<T, std::exception_ptr>;

/// State for detached/use_awaitable mode (no completion handler).
/// Uses type-erased unique_function to avoid storing lambda types directly.
template <typename T>
struct spawn_state {
  unique_function<awaitable<T>()> factory{};

  template <typename F>
    requires std::is_invocable_r_v<awaitable<T>, F&>
  explicit spawn_state(F&& f) : factory(std::forward<F>(f)) {}
};

/// State for completion callback mode.
/// Both factory and completion are type-erased.
template <typename T>
struct spawn_state_with_completion {
  unique_function<awaitable<T>()> factory{};
  unique_function<void(spawn_expected<T>)> completion{};

  template <typename F, typename C>
    requires std::is_invocable_r_v<awaitable<T>, F&> && std::is_invocable_v<C&, spawn_expected<T>>
  spawn_state_with_completion(F&& f, C&& c)
      : factory(std::forward<F>(f)), completion(std::forward<C>(c)) {}
};

/// Helper to safely invoke completion callback, swallowing any exceptions.
template <typename F, typename T>
void safe_invoke_completion(F& completion, spawn_expected<T> result) noexcept {
  try {
    completion(std::move(result));
  } catch (...) {
    // Completion callback exceptions are swallowed (detached semantics).
  }
}

/// Unified coroutine entry point for co_spawn.
///
/// This is the only coroutine wrapper responsible for owning and invoking the user-supplied
/// callable (or an awaitable wrapped as a callable).
template <typename T>
auto spawn_entry_point(std::shared_ptr<spawn_state<T>> state) -> awaitable<T> {
  co_return co_await state->factory();
}

template <typename T>
auto spawn_entry_point_with_completion(std::shared_ptr<spawn_state_with_completion<T>> state)
  -> awaitable<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await state->factory();
      safe_invoke_completion(state->completion, spawn_expected<void>{});
    } else {
      auto v = co_await state->factory();
      safe_invoke_completion(state->completion, spawn_expected<T>{std::move(v)});
    }
  } catch (...) {
    auto ep = std::current_exception();
    safe_invoke_completion(state->completion, spawn_expected<T>{unexpected(ep)});
  }
  co_return;
}

template <typename T>
auto bind_executor(any_executor ex, awaitable<T> a) -> awaitable<T> {
  auto h = a.release();
  h.promise().set_executor(std::move(ex));
  return awaitable<T>{h};
}

template <typename T>
void spawn_detached_impl(any_executor ex, awaitable<T> a) {
  auto h = a.release();

  h.promise().set_executor(ex);
  h.promise().detach();

  ex.post([h, ex]() mutable {
    executor_guard g{ex};
    try {
      h.resume();
    } catch (...) {
      // Detached mode: swallow exceptions
    }
  });
}

template <typename T>
struct spawn_wait_state {
  any_executor ex{};
  std::mutex m;
  bool done{false};
  std::coroutine_handle<> waiter{};
  std::exception_ptr ep{};
  [[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>>
    value{};

  explicit spawn_wait_state(any_executor ex_) : ex(std::move(ex_)) {}

  // Overload for non-void types (takes parameter)
  template <typename U = T>
  void set_value(U v)
    requires(!std::is_void_v<T> && std::is_same_v<U, T>)
  {
    std::scoped_lock lk{m};
    value.emplace(std::move(v));
  }

  // Overload for void type (no parameter)
  void set_value() noexcept
    requires std::is_void_v<T>
  {}

  void set_exception(std::exception_ptr e) {
    std::scoped_lock lk{m};
    ep = std::move(e);
  }

  void complete() {
    std::coroutine_handle<> w{};
    {
      std::scoped_lock lk{m};
      done = true;
      w = waiter;
      waiter = {};
    }
    if (w) {
      ex.post([w]() { w.resume(); });
    }
  }
};

template <typename T>
struct state_awaiter {
  // IMPORTANT: Explicit constructor required to avoid ASan use-after-free errors.
  //
  // Issue: Aggregate initialization `awaiter{shared_ptr}` triggers ASan
  // use-after-free errors. Known to affect:
  // - state_awaiter<T> / state_awaiter<void> (this file)
  // - awaiter inside steady_timer::async_wait (steady_timer.ipp)
  // - when_all_awaiter (when_all/state.hpp)
  // Likely affects all awaiters containing shared_ptr members.
  //
  // Suspected cause: Aggregate init may create temporaries with incorrect
  // lifetime, leading to premature destruction of the shared state.
  //
  // gcc version: 12.2.0
  //
  // Workaround: Explicit constructor with std::move ensures proper ownership
  // transfer and resolves all ASan issues.
  //
  // TODO: Investigate root cause - possibly related to:
  // - C++17/20 aggregate initialization rules changes
  // - Compiler-specific temporary materialization behavior
  // - Interaction between move semantics and aggregate members
  explicit state_awaiter(std::shared_ptr<spawn_wait_state<T>> st_) : st(std::move(st_)) {}

  std::shared_ptr<spawn_wait_state<T>> st;

  // Always suspend and resume via executor to match the library's "never inline" policy.
  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    bool ready = false;
    {
      std::scoped_lock lk{st->m};
      IOCORO_ENSURE(!st->waiter, "co_spawn(use_awaitable): multiple awaiters are not supported");
      ready = st->done;
      if (!ready) {
        st->waiter = h;
      }
    }
    if (ready) {
      st->ex.post([h]() { h.resume(); });
    }
  }

  auto await_resume() -> T {
    std::exception_ptr ep{};
    if constexpr (!std::is_void_v<T>) {
      std::optional<T> v{};
      {
        std::scoped_lock lk{st->m};
        ep = st->ep;
        v = std::move(st->value);
        st->value.reset();
      }
      if (ep) {
        std::rethrow_exception(ep);
      }
      IOCORO_ENSURE(v.has_value(), "co_spawn(use_awaitable): missing value");
      return std::move(*v);
    } else {
      {
        std::scoped_lock lk{st->m};
        ep = st->ep;
      }
      if (ep) {
        std::rethrow_exception(ep);
      }
    }
  }
};

template <typename T>
auto await_state(std::shared_ptr<spawn_wait_state<T>> st) -> awaitable<T> {
  co_return co_await state_awaiter<T>{std::move(st)};
}

template <typename T>
auto run_to_state(any_executor ex, std::shared_ptr<spawn_wait_state<T>> st, awaitable<T> a)
  -> awaitable<void> {
  auto bound = bind_executor<T>(ex, std::move(a));
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(bound);
      st->set_value();
    } else {
      st->set_value(co_await std::move(bound));
    }
  } catch (...) {
    st->set_exception(std::current_exception());
  }
  st->complete();
}

}  // namespace iocoro::detail
