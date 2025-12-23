#pragma once

#include <xz/io/detail/when_common/state_base.hpp>

#include <cstddef>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace xz::io::detail {

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

}  // namespace xz::io::detail
