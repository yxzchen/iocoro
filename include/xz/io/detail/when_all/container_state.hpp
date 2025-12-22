#pragma once

#include <xz/io/detail/when_all/state.hpp>

#include <vector>

namespace xz::io::detail {

template <class T>
struct when_all_container_state {
  using value_t = when_all_value_t<T>;

  executor ex{};
  std::mutex m;
  std::atomic<std::size_t> remaining{0};
  std::coroutine_handle<> waiter{};
  std::exception_ptr first_ep{};

  // Only used for non-void T
  std::vector<std::optional<value_t>> values{};

  when_all_container_state(executor ex_, std::size_t n) : ex(ex_) {
    remaining.store(n, std::memory_order_relaxed);
    if constexpr (!std::is_void_v<T>) {
      values.resize(n);
    }
  }

  void set_exception(std::exception_ptr ep) noexcept {
    std::scoped_lock lk{m};
    if (!first_ep) first_ep = std::move(ep);
  }

  void set_value(std::size_t i, value_t v) {
    static_assert(!std::is_void_v<T>);
    std::scoped_lock lk{m};
    XZ_ENSURE(i < values.size(), "when_all(vector): index out of range");
    values[i].emplace(std::move(v));
  }

  void arrive() noexcept {
    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) complete();
  }

  void complete() noexcept {
    std::coroutine_handle<> w{};
    {
      std::scoped_lock lk{m};
      w = waiter;
      waiter = {};
    }
    if (w) {
      ex.post([w, ex = ex]() mutable {
        executor_guard g{ex};
        w.resume();
      });
    }
  }
};

}  // namespace xz::io::detail
