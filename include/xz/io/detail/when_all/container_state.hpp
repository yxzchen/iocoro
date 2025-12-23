#pragma once

#include <xz/io/detail/when_all/state.hpp>

#include <vector>

namespace xz::io::detail {

template <class T>
struct when_all_container_state : when_all_state_base<when_all_container_state<T>> {
  using value_t = when_all_value_t<T>;

  // Only used for non-void T
  std::vector<std::optional<value_t>> values{};

  when_all_container_state(executor ex_, std::size_t n)
    : when_all_state_base<when_all_container_state<T>>(ex_, n) {
    if constexpr (!std::is_void_v<T>) {
      values.resize(n);
    }
  }

  void set_value(std::size_t i, value_t v) {
    static_assert(!std::is_void_v<T>);
    std::scoped_lock lk{this->m};
    XZ_ENSURE(i < values.size(), "when_all(vector): index out of range");
    values[i].emplace(std::move(v));
  }
};

}  // namespace xz::io::detail
