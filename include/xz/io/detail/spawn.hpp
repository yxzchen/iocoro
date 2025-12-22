#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/awaitable.hpp>
#include <xz/io/detached.hpp>
#include <xz/io/detail/executor_guard.hpp>
#include <xz/io/executor.hpp>
#include <xz/io/expected.hpp>
#include <xz/io/use_awaitable.hpp>

#include <atomic>
#include <concepts>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace xz::io::detail {

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

template <typename T>
struct spawn_state {
  executor ex{};
  std::mutex m;
  std::atomic<bool> done{false};
  std::coroutine_handle<> waiter{};
  std::exception_ptr ep{};
  std::optional<T> value{};

  explicit spawn_state(executor ex_) : ex(ex_) {}

  void set_value(T v) {
    std::scoped_lock lk{m};
    value.emplace(std::move(v));
  }

  void set_exception(std::exception_ptr e) {
    std::scoped_lock lk{m};
    ep = std::move(e);
  }

  void complete() {
    std::coroutine_handle<> w{};
    {
      std::scoped_lock lk{m};
      done.store(true, std::memory_order_release);
      w = waiter;
      waiter = {};
    }
    if (w) {
      ex.post([w, ex = ex]() mutable {
        executor_guard g{ex};
        w.resume();
      });
    }
  }
};

template <>
struct spawn_state<void> {
  executor ex{};
  std::mutex m;
  std::atomic<bool> done{false};
  std::coroutine_handle<> waiter{};
  std::exception_ptr ep{};

  explicit spawn_state(executor ex_) : ex(ex_) {}

  void set_value() noexcept {}

  void set_exception(std::exception_ptr e) {
    std::scoped_lock lk{m};
    ep = std::move(e);
  }

  void complete() {
    std::coroutine_handle<> w{};
    {
      std::scoped_lock lk{m};
      done.store(true, std::memory_order_release);
      w = waiter;
      waiter = {};
    }
    if (w) {
      ex.post([w, ex = ex]() mutable {
        executor_guard g{ex};
        w.resume();
      });
    }
  }
};

template <typename T>
struct state_awaiter {
  explicit state_awaiter(std::shared_ptr<spawn_state<T>> st_) : st(std::move(st_)) {}

  std::shared_ptr<spawn_state<T>> st;

  // Always suspend and resume via executor to match the library's "never inline" policy.
  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    bool ready = false;
    {
      std::scoped_lock lk{st->m};
      XZ_ENSURE(!st->waiter, "co_spawn(use_awaitable): multiple awaiters are not supported");
      ready = st->done.load(std::memory_order_acquire);
      if (!ready) st->waiter = h;
    }
    if (ready) {
      st->ex.post([h, ex = st->ex]() mutable {
        executor_guard g{ex};
        h.resume();
      });
    }
  }

  auto await_resume() -> T {
    std::exception_ptr ep{};
    std::optional<T> v{};
    {
      std::scoped_lock lk{st->m};
      ep = st->ep;
      v = std::move(st->value);
      st->value.reset();
    }
    if (ep) std::rethrow_exception(ep);
    XZ_ENSURE(v.has_value(), "co_spawn(use_awaitable): missing value");
    return std::move(*v);
  }
};

template <>
struct state_awaiter<void> {
  explicit state_awaiter(std::shared_ptr<spawn_state<void>> st_) : st(std::move(st_)) {}

  std::shared_ptr<spawn_state<void>> st;

  // Always suspend and resume via executor to match the library's "never inline" policy.
  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    bool ready = false;
    {
      std::scoped_lock lk{st->m};
      XZ_ENSURE(!st->waiter, "co_spawn(use_awaitable): multiple awaiters are not supported");
      ready = st->done.load(std::memory_order_acquire);
      if (!ready) st->waiter = h;
    }
    if (ready) {
      st->ex.post([h, ex = st->ex]() mutable {
        executor_guard g{ex};
        h.resume();
      });
    }
  }

  void await_resume() {
    std::exception_ptr ep{};
    {
      std::scoped_lock lk{st->m};
      ep = st->ep;
    }
    if (ep) std::rethrow_exception(ep);
  }
};

template <typename T>
auto await_state(std::shared_ptr<spawn_state<T>> st) -> awaitable<T> {
  if constexpr (std::is_void_v<T>) {
    co_await state_awaiter<void>{std::move(st)};
    co_return;
  } else {
    co_return co_await state_awaiter<T>{std::move(st)};
  }
}

template <typename T>
auto run_to_state(executor ex, std::shared_ptr<spawn_state<T>> st, awaitable<T> a) -> awaitable<void> {
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

}  // namespace xz::io::detail
