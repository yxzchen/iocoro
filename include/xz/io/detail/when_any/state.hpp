#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/awaitable.hpp>
#include <xz/io/detail/executor_guard.hpp>
#include <xz/io/executor.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace xz::io::detail {

template <class T>
struct when_any_value {
  using type = std::conditional_t<std::is_void_v<T>, std::monostate, std::remove_cvref_t<T>>;
};

template <class T>
using when_any_value_t = typename when_any_value<T>::type;

// Shared state base for when_any (variadic + container).
// Similar to when_all_state_base but for "first to complete" semantics.
template <class Derived>
struct when_any_state_base {
  executor ex{};
  std::mutex m;
  std::atomic<bool> completed{false};  // First-complete flag
  std::coroutine_handle<> waiter{};
  std::exception_ptr first_ep{};

  explicit when_any_state_base(executor ex_) : ex(ex_) {}

  void set_exception(std::exception_ptr ep) noexcept {
    std::scoped_lock lk{m};
    if (!first_ep) {
      first_ep = std::move(ep);
    }
  }

  // Try to mark as completed. Returns true if this is the first to complete.
  bool try_complete() noexcept {
    return !completed.exchange(true, std::memory_order_acq_rel);
  }

  void complete() noexcept {
    std::coroutine_handle<> w{};
    {
      std::scoped_lock lk{m};
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

template <class... Ts>
struct when_any_state : when_any_state_base<when_any_state<Ts...>> {
  using values_variant = std::variant<std::monostate, std::optional<when_any_value_t<Ts>>...>;

  std::mutex result_m;
  std::size_t completed_index{0};
  values_variant result{};

  explicit when_any_state(executor ex_)
    : when_any_state_base<when_any_state<Ts...>>(ex_) {}

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{result_m};
    completed_index = I;
    result.template emplace<I + 1>(std::forward<V>(v));
  }
};

template <class State>
struct when_any_awaiter {
  explicit when_any_awaiter(std::shared_ptr<State> s) : st(s) {}

  std::shared_ptr<State> st;

  bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<> h) {
    std::scoped_lock lk{st->m};
    XZ_ENSURE(!st->waiter, "when_any: multiple awaiters are not supported");
    if (st->completed.load(std::memory_order_relaxed)) {
      return false;  // already completed; resume immediately
    }
    st->waiter = h;
    return true;
  }

  void await_resume() noexcept {}
};

template <class State>
auto await_when_any(std::shared_ptr<State> st) -> ::xz::io::awaitable<void> {
  co_await when_any_awaiter<State>{std::move(st)};
}

}  // namespace xz::io::detail
