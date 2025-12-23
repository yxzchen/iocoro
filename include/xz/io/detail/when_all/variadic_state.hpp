#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/awaitable.hpp>
#include <xz/io/detail/when_all/state_base.hpp>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace xz::io::detail {

template <class... Ts>
struct when_all_state : when_all_state_base<when_all_state<Ts...>> {
  using values_tuple = std::tuple<std::optional<when_all_value_t<Ts>>...>;

  values_tuple values{};

  explicit when_all_state(executor ex_)
    : when_all_state_base<when_all_state<Ts...>>(ex_, sizeof...(Ts)) {}

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{this->m};
    std::get<I>(values).emplace(std::forward<V>(v));
  }
};

}  // namespace xz::io::detail

