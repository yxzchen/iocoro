#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/detail/when/when_state_base.hpp>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace iocoro::detail {

template <class... Ts>
struct when_any_variadic_state : when_state_base {
  using values_variant = std::variant<std::monostate, std::optional<when_value_t<Ts>>...>;

  std::size_t completed_index{0};
  values_variant result{};

  explicit when_any_variadic_state() : when_state_base(0) {}

  bool try_complete() noexcept { return !done.exchange(true, std::memory_order_acq_rel); }
  bool is_done() const noexcept { return done.load(std::memory_order_acquire); }

  template <std::size_t I, class V>
  void set_value(V&& v) {
    std::scoped_lock lk{this->m};
    completed_index = I;
    result.template emplace<I + 1>(std::forward<V>(v));
  }

 private:
  std::atomic<bool> done{false};
};

template <class T>
struct when_any_container_state : when_state_base {
  using value_t = when_value_t<T>;

  std::size_t completed_index{0};
  std::optional<value_t> result{};

  explicit when_any_container_state() : when_state_base(0) {}

  bool try_complete() noexcept { return !done.exchange(true, std::memory_order_acq_rel); }
  bool is_done() const noexcept { return done.load(std::memory_order_acquire); }

  void set_value(std::size_t i, value_t v) {
    static_assert(!std::is_void_v<T>);
    std::scoped_lock lk{this->m};
    completed_index = i;
    result.emplace(std::move(v));
  }

  void set_void_result(std::size_t i) {
    static_assert(std::is_void_v<T>);
    std::scoped_lock lk{this->m};
    completed_index = i;
  }

 private:
  std::atomic<bool> done{false};
};

}  // namespace iocoro::detail
