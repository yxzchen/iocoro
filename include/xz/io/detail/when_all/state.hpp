#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/awaitable.hpp>
#include <xz/io/detail/executor_guard.hpp>
#include <xz/io/executor.hpp>

#include <atomic>
#include <coroutine>
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
struct when_all_value {
  using type = std::conditional_t<std::is_void_v<T>, std::monostate, std::remove_cvref_t<T>>;
};

template <class T>
using when_all_value_t = typename when_all_value<T>::type;

template <class... Ts>
struct when_all_state {
  using values_tuple = std::tuple<std::optional<when_all_value_t<Ts>>...>;

  executor ex{};
  std::mutex m;
  std::atomic<std::size_t> remaining{0};
  std::coroutine_handle<> waiter{};
  std::exception_ptr first_ep{};
  values_tuple values{};

  explicit when_all_state(executor ex_) : ex(ex_) {
    remaining.store(sizeof...(Ts), std::memory_order_relaxed);
  }

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{m};
    std::get<I>(values).emplace(std::forward<V>(v));
  }

  void set_exception(std::exception_ptr ep) noexcept {
    std::scoped_lock lk{m};
    if (!first_ep) first_ep = std::move(ep);
  }

  void arrive() noexcept {
    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) complete();
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

template <class State>
struct when_all_awaiter {
  std::shared_ptr<State> st;

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    bool ready = false;
    {
      std::scoped_lock lk{st->m};
      XZ_ENSURE(!st->waiter, "when_all: multiple awaiters are not supported");
      ready = (st->remaining.load(std::memory_order_acquire) == 0);
      if (!ready) st->waiter = h;
    }
    if (ready) {
      st->ex.post([h, ex = st->ex]() mutable {
        executor_guard g{ex};
        h.resume();
      });
    }
  }

  void await_resume() noexcept {}
};

template <class State>
auto await_when_all(std::shared_ptr<State> st) -> ::xz::io::awaitable<void> {
  co_await when_all_awaiter<State>{std::move(st)};
}

}  // namespace xz::io::detail
