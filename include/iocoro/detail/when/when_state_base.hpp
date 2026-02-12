#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>

namespace iocoro::detail {

template <class T>
using when_value_t = std::conditional_t<std::is_void_v<T>, std::monostate, std::remove_cvref_t<T>>;

// Shared state base for fan-in combinators.
// - Counting mode (default): remaining = n (number of completions to wait for).
// - One-shot mode can be implemented by derived states overriding try_complete()/is_done().
struct when_state_base {
  any_executor ex{};
  std::mutex m;
  std::atomic<std::size_t> remaining{0};
  std::exception_ptr first_ep{};
  std::atomic<void*> waiter_addr{nullptr};

  static auto done_sentinel() noexcept -> void* {
    // Coroutine frame addresses are at least pointer-aligned; reserve 1.
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
  }

  explicit when_state_base(std::size_t n) { remaining.store(n, std::memory_order_relaxed); }

  bool is_done() const noexcept { return (remaining.load(std::memory_order_acquire) == 0); }

  void set_exception(std::exception_ptr ep) noexcept {
    std::scoped_lock lk{m};
    if (!first_ep) {
      first_ep = std::move(ep);
    }
  }

  bool try_complete() noexcept {
    auto old = remaining.load(std::memory_order_acquire);
    while (old != 0) {
      if (remaining.compare_exchange_weak(old, old - 1, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return (old == 1);
      }
    }
    return false;
  }

  void complete() noexcept {
    // Publish completion and, if a waiter exists, resume it exactly once.
    void* addr = waiter_addr.load(std::memory_order_acquire);
    for (;;) {
      if (addr == done_sentinel()) {
        return;
      }
      if (addr == nullptr) {
        if (waiter_addr.compare_exchange_weak(addr, done_sentinel(), std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          return;
        }
        continue;
      }
      if (waiter_addr.compare_exchange_weak(addr, done_sentinel(), std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        break;
      }
    }

    auto w = std::coroutine_handle<>::from_address(addr);
    if (!w) {
      return;
    }

    any_executor resume_ex{};
    {
      std::scoped_lock lk{m};
      resume_ex = ex;
    }
    IOCORO_ENSURE(resume_ex, "when_all/when_any: empty executor for resume");
    resume_ex.post([w]() mutable noexcept { w.resume(); });
  }
};

template <class State>
struct when_awaiter {
  explicit when_awaiter(std::shared_ptr<State> st_) : st(st_) {}

  std::shared_ptr<State> st;

  bool await_ready() const noexcept { return st->is_done(); }

  template <class Promise>
    requires requires(Promise& p) { p.get_executor(); }
  bool await_suspend(std::coroutine_handle<Promise> h) {
    if (st->is_done()) {
      return false;
    }

    auto ex = h.promise().get_executor();
    IOCORO_ENSURE(ex, "when_all/when_any: empty executor");
    {
      std::scoped_lock lk{st->m};
      st->ex = ex;
    }

    void* expected = nullptr;
    void* desired = h.address();
    IOCORO_ENSURE(desired != when_state_base::done_sentinel(),
                  "when_all/when_any: invalid coroutine address");

    if (!st->waiter_addr.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
      // Either already completed (sentinel) or multiple awaiters (non-null).
      if (expected == when_state_base::done_sentinel()) {
        return false;
      }
      IOCORO_ENSURE(false, "when_all/when_any: multiple awaiters are not supported");
    }

    return true;
  }

  void await_resume() noexcept {}
};

template <class State>
auto await_when(std::shared_ptr<State> st) -> awaitable<void> {
  co_await when_awaiter<State>{std::move(st)};
}

}  // namespace iocoro::detail
