#pragma once

#include <utility>

namespace iocoro::detail {

template <class F>
class scope_exit {
 public:
  explicit scope_exit(F f) noexcept : f_(std::move(f)), active_(true) {}
  scope_exit(scope_exit const&) = delete;
  auto operator=(scope_exit const&) -> scope_exit& = delete;
  scope_exit(scope_exit&& other) noexcept : f_(std::move(other.f_)), active_(other.active_) {
    other.active_ = false;
  }
  auto operator=(scope_exit&& other) noexcept -> scope_exit& {
    if (this != &other) {
      if (active_) {
        f_();
      }
      f_ = std::move(other.f_);
      active_ = other.active_;
      other.active_ = false;
    }
    return *this;
  }
  ~scope_exit() {
    if (active_) {
      f_();
    }
  }

  void release() noexcept { active_ = false; }

 private:
  F f_;
  bool active_;
};

template <class F>
[[nodiscard]] auto make_scope_exit(F f) noexcept -> scope_exit<F> {
  return scope_exit<F>(std::move(f));
}

}  // namespace iocoro::detail
