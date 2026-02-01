#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/detail/when/when_state_base.hpp>
#include <iocoro/this_coro.hpp>

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace iocoro {

namespace detail {

template <class A, class B>
struct when_or_state : when_state_base {
  using result_variant = std::variant<when_value_t<A>, when_value_t<B>>;

  std::mutex result_m{};
  std::optional<result_variant> result{};
  std::size_t completed_index{0};
  awaitable<A> task_a;
  awaitable<B> task_b;

  explicit when_or_state(awaitable<A> a, awaitable<B> b)
      : when_state_base(1), task_a(std::move(a)), task_b(std::move(b)) {}

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{result_m};
    completed_index = I;
    result.emplace(std::in_place_index<I>, std::forward<V>(v));
  }

  void request_cancel_other(std::size_t index) noexcept {
    if (index == 0) {
      task_b.request_stop();
      return;
    }
    task_a.request_stop();
  }
};

template <std::size_t I, class T, class A, class B>
auto when_or_run_one(std::shared_ptr<when_or_state<A, B>> st) -> awaitable<void> {
  try {
    if constexpr (std::is_void_v<T>) {
      if constexpr (I == 0) {
        co_await st->task_a;
      } else {
        co_await st->task_b;
      }
      if (st->try_complete()) {
        st->request_cancel_other(I);
        st->template set_value<I>(std::monostate{});
        st->complete();
      }
    } else {
      if constexpr (I == 0) {
        auto result = co_await st->task_a;
        if (st->try_complete()) {
          st->request_cancel_other(I);
          st->template set_value<I>(std::move(result));
          st->complete();
        }
      } else {
        auto result = co_await st->task_b;
        if (st->try_complete()) {
          st->request_cancel_other(I);
          st->template set_value<I>(std::move(result));
          st->complete();
        }
      }
    }
  } catch (...) {
    if (st->try_complete()) {
      st->request_cancel_other(I);
      st->set_exception(std::current_exception());
      st->complete();
    }
  }
  co_return;
}

}  // namespace detail

template <class A, class B>
auto operator||(awaitable<A> a, awaitable<B> b)
  -> awaitable<std::pair<std::size_t, std::variant<detail::when_value_t<A>, detail::when_value_t<B>>>> {
  auto fallback_ex = co_await this_coro::executor;
  IOCORO_ENSURE(fallback_ex, "operator||: requires a bound executor");

  auto st = std::make_shared<detail::when_or_state<A, B>>(std::move(a), std::move(b));

  auto ex_a = st->task_a.get_executor();
  detail::spawn_task<void>(
    detail::spawn_context{ex_a ? ex_a : fallback_ex},
    [st]() mutable -> awaitable<void> {
      return detail::when_or_run_one<0, A, A, B>(st);
    },
    detail::detached_completion<void>{});

  auto ex_b = st->task_b.get_executor();
  detail::spawn_task<void>(
    detail::spawn_context{ex_b ? ex_b : fallback_ex},
    [st]() mutable -> awaitable<void> {
      return detail::when_or_run_one<1, B, A, B>(st);
    },
    detail::detached_completion<void>{});

  co_await detail::await_when(st);

  std::exception_ptr ep{};
  std::size_t index{};
  std::optional<typename detail::when_or_state<A, B>::result_variant> result{};
  {
    std::scoped_lock lk{st->result_m};
    ep = st->first_ep;
    if (!ep) {
      index = st->completed_index;
      result = std::move(st->result);
    }
  }

  if (ep) {
    std::rethrow_exception(ep);
  }

  IOCORO_ENSURE(result.has_value(), "operator||: missing result");
  co_return std::make_pair(index, std::move(*result));
}

}  // namespace iocoro
