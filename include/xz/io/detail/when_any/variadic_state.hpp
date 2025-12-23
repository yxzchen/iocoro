#pragma once

#include <xz/io/assert.hpp>
#include <xz/io/awaitable.hpp>
#include <xz/io/detail/when_any/state_base.hpp>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace xz::io::detail {

template <class... Ts>
struct when_any_state : when_any_state_base<when_any_state<Ts...>> {
  using values_variant = std::variant<std::monostate, std::optional<when_any_value_t<Ts>>...>;

  std::mutex result_m;
  std::size_t completed_index{0};
  values_variant result{};

  explicit when_any_state(executor ex_) : when_any_state_base<when_any_state<Ts...>>(ex_) {}

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{result_m};
    completed_index = I;
    result.template emplace<I + 1>(std::forward<V>(v));
  }
};

}  // namespace xz::io::detail
