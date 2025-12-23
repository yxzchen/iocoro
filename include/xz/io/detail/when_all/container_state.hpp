#pragma once

#include <xz/io/detail/when_common/state_base.hpp>

#include <cstddef>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace xz::io::detail {

template <class T>
struct when_all_container_state : when_state_base<when_all_container_state<T>> {
  using value_t = when_value_t<T>;

  // Only used for non-void T
  std::vector<std::optional<value_t>> values{};

  when_all_container_state(executor ex_, std::size_t n)
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
