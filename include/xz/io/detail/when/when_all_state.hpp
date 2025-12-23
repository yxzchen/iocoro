#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/awaitable.hpp>
#include <xz/io/detail/when/state_base.hpp>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace xz::io::detail {

template <class... Ts>
struct when_all_variadic_state : when_state_base<when_all_variadic_state<Ts...>> {
  using values_tuple = std::tuple<std::optional<when_value_t<Ts>>...>;

  values_tuple values{};

  explicit when_all_variadic_state(executor ex_)
      : when_state_base<when_all_variadic_state<Ts...>>(ex_, sizeof...(Ts)) {}

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{this->m};
    std::get<I>(values).emplace(std::forward<V>(v));
  }
};

template <class T>
struct when_all_container_state : when_state_base<when_all_container_state<T>> {
  using value_t = when_value_t<T>;

  // Only used for non-void T
  std::vector<std::optional<value_t>> values{};

  explicit when_all_container_state(executor ex_, std::size_t n)
      : when_state_base<when_all_container_state<T>>(ex_, n) {
    if constexpr (!std::is_void_v<T>) {
      values.resize(n);
    }
  }

  void set_value(std::size_t i, value_t v) {
    static_assert(!std::is_void_v<T>);
    std::scoped_lock lk{this->m};
    values[i].emplace(std::move(v));
  }
};

}  // namespace xz::io::detail
