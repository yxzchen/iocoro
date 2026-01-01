#pragma once

#include <iocoro/executor.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <mutex>
#include <utility>

namespace iocoro::detail {

template <class T>
using when_value_t = std::conditional_t<std::is_void_v<T>, std::monostate, std::remove_cvref_t<T>>;

// Shared state base for when_all/when_any.
// - when_all: remaining = n (number of all tasks)
// - when_any: remaining = 1 (only one completion needed)
struct when_state_base {
  any_executor ex{};
  std::mutex m;
  std::atomic<std::size_t> remaining{0};
  std::coroutine_handle<> waiter{};
  std::exception_ptr first_ep{};

  explicit when_state_base(std::size_t n) {
    remaining.store(n, std::memory_order_relaxed);
  }

  void set_exception(std::exception_ptr ep) noexcept {
    std::scoped_lock lk{m};
    if (!first_ep) {
      first_ep = std::move(ep);
    }
  }

  bool try_complete() noexcept { return (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1); }

  void complete() noexcept {
    std::coroutine_handle<> w{};
    {
      std::scoped_lock lk{m};
      w = waiter;
      waiter = {};
    }
    if (w) {
      ex.post([w]() { w.resume(); });
    }
  }
};

template <class State>
struct when_awaiter {
  explicit when_awaiter(std::shared_ptr<State> st_) : st(st_) {}

  std::shared_ptr<State> st;

  bool await_ready() const noexcept {
    return (st->remaining.load(std::memory_order_relaxed) == 0);
  }

  bool await_suspend(std::coroutine_handle<> h) {
    std::scoped_lock lk{st->m};
    if (st->remaining.load(std::memory_order_relaxed) == 0) {
      return false;
    }
    
    IOCORO_ENSURE(!st->waiter, "when_all/when_any: multiple awaiters are not supported");
    st->waiter = h;
    st->ex = get_current_executor();
    return true;
  }

  void await_resume() noexcept {}
};

template <class State>
auto await_when(std::shared_ptr<State> st) -> awaitable<void> {
  co_await when_awaiter<State>{std::move(st)};
}

}  // namespace iocoro::detail
