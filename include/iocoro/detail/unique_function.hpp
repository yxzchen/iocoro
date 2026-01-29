#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace iocoro::detail {

// TODO: rewrite using vtable.

/// Move-only type-erased callable wrapper.
///
/// This avoids storing lambda closure types in coroutine frames, eliminating
/// -Wsubobject-linkage warnings.
template <typename>
class unique_function;

template <typename R, typename... Args>
class unique_function<R(Args...)> {
 public:
  unique_function() = default;

  template <typename F>
    requires (!std::is_same_v<std::decay_t<F>, unique_function>) &&
             std::is_invocable_r_v<R, F&, Args...>
  unique_function(F&& f) : ptr_(std::make_unique<model<std::decay_t<F>>>(std::forward<F>(f))) {}

  unique_function(unique_function&&) noexcept = default;
  auto operator=(unique_function&&) noexcept -> unique_function& = default;

  unique_function(unique_function const&) = delete;
  auto operator=(unique_function const&) -> unique_function& = delete;

  explicit operator bool() const noexcept { return static_cast<bool>(ptr_); }

  auto operator()(Args... args) -> R {
    assert(ptr_);
    return ptr_->invoke(std::forward<Args>(args)...);
  }

 private:
  struct callable_base {
    virtual ~callable_base() = default;
    virtual auto invoke(Args... args) -> R = 0;
  };

  template <typename F>
  struct model final : callable_base {
    F f_;

    template <typename U>
    explicit model(U&& f) : f_(std::forward<U>(f)) {}

    auto invoke(Args... args) -> R override { return std::invoke(f_, std::forward<Args>(args)...); }
  };

  std::unique_ptr<callable_base> ptr_{};
};

}  // namespace iocoro::detail


