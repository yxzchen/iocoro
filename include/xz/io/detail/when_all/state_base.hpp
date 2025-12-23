#pragma once

#include <xz/io/detail/executor_guard.hpp>
#include <xz/io/executor.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <mutex>
#include <utility>

namespace xz::io::detail {

template <class T>
struct when_all_value {
  using type = std::conditional_t<std::is_void_v<T>, std::monostate, std::remove_cvref_t<T>>;
};

template <class T>
using when_all_value_t = typename when_all_value<T>::type;

// Shared state base for when_all (variadic + container).
template <class Derived>
struct when_all_state_base {
  executor ex{};
  std::mutex m;
  std::atomic<std::size_t> remaining{0};
  std::coroutine_handle<> waiter{};
  std::exception_ptr first_ep{};

  when_all_state_base(executor ex_, std::size_t n) : ex(ex_) {
    remaining.store(n, std::memory_order_relaxed);
  }

  void set_exception(std::exception_ptr ep) noexcept {
    std::scoped_lock lk{m};
    if (!first_ep) {
      first_ep = std::move(ep);
    }
  }

  void arrive() noexcept {
    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      complete();
    }
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
  explicit when_all_awaiter(std::shared_ptr<State> s) : st(s) {}

  std::shared_ptr<State> st;

  bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<> h) {
    std::scoped_lock lk{st->m};
    XZ_ENSURE(!st->waiter, "when_all: multiple awaiters are not supported");
    if (st->remaining.load(std::memory_order_relaxed) == 0) {
      return false;  // already completed; resume immediately
    }
    st->waiter = h;
    return true;
  }

  void await_resume() noexcept {}
};

template <class State>
auto await_when_all(std::shared_ptr<State> st) -> ::xz::io::awaitable<void> {
  co_await when_all_awaiter<State>{std::move(st)};
}

}  // namespace xz::io::detail

