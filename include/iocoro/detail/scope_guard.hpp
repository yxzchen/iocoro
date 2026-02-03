#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace iocoro::detail {

template <class F>
  requires std::invocable<F&>
class [[nodiscard]] scope_exit {
 public:
  explicit scope_exit(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
      : f_(std::move(f)), active_(true) {}
  scope_exit(scope_exit const&) = delete;
  auto operator=(scope_exit const&) -> scope_exit& = delete;
  scope_exit(scope_exit&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
      : f_(std::move(other.f_)), active_(std::exchange(other.active_, false)) {}
  auto operator=(scope_exit&& other) noexcept(std::is_nothrow_move_assignable_v<F> &&
                                              std::is_nothrow_move_constructible_v<F>)
    -> scope_exit& {
    if (this != &other) {
      run();
      f_ = std::move(other.f_);
      active_ = std::exchange(other.active_, false);
    }
    return *this;
  }
  ~scope_exit() noexcept { run(); }

  void release() noexcept { active_ = false; }

 private:
  void run() noexcept {
    if (!active_) {
      return;
    }
    active_ = false;
    try {
      std::invoke(f_);
    } catch (...) {
    }
  }

  [[no_unique_address]] F f_;
  bool active_;
};

template <class F>
[[nodiscard]] auto make_scope_exit(F&& f) noexcept(
  std::is_nothrow_constructible_v<std::decay_t<F>, F>) -> scope_exit<std::decay_t<F>> {
  return scope_exit<std::decay_t<F>>(std::forward<F>(f));
}

}  // namespace iocoro::detail
