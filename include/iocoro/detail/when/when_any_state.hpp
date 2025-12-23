#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/detail/when/state_base.hpp>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace iocoro::detail {

template <class... Ts>
struct when_any_variadic_state : when_state_base<when_any_variadic_state<Ts...>> {
  using values_variant = std::variant<std::monostate, std::optional<when_value_t<Ts>>...>;

  std::mutex result_m;
  std::size_t completed_index{0};
  values_variant result{};

  explicit when_any_variadic_state(executor ex_) : when_state_base<when_any_variadic_state<Ts...>>(ex_, 1) {}

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{result_m};
    completed_index = I;
    result.template emplace<I + 1>(std::forward<V>(v));
  }
};

template <class T>
struct when_any_container_state : when_state_base<when_any_container_state<T>> {
  using value_t = when_value_t<T>;

  std::mutex result_m;
  std::size_t completed_index{0};
  std::optional<value_t> result{};

  explicit when_any_container_state(executor ex_)
      : when_state_base<when_any_container_state<T>>(ex_, 1) {}

  void set_value(std::size_t i, value_t v) {
    static_assert(!std::is_void_v<T>);
    std::scoped_lock lk{result_m};
    completed_index = i;
    result.emplace(std::move(v));
  }

  void set_void_result(std::size_t i) {
    static_assert(std::is_void_v<T>);
    std::scoped_lock lk{result_m};
    completed_index = i;
  }
};

}  // namespace iocoro::detail
