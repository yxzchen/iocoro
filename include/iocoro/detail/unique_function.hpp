#pragma once

#include <iocoro/assert.hpp>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace iocoro::detail {

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
 ~unique_function() { reset(); }

  template <typename F>
    requires (!std::is_same_v<std::decay_t<F>, unique_function>) &&
             (std::is_invocable_r_v<R, F&, Args...> ||
              std::is_invocable_r_v<R, F const&, Args...>)
  unique_function(F&& f) {
    using functor = std::decay_t<F>;
    auto* p = new functor(std::forward<F>(f));
    ptr_ = p;
    vtable_ = &vtable_for<functor>;
  }

  unique_function(unique_function&& other) noexcept { move_from(other); }
  auto operator=(unique_function&& other) noexcept -> unique_function& {
    if (this != &other) {
      reset();
      move_from(other);
    }
    return *this;
  }

  unique_function(unique_function const&) = delete;
  auto operator=(unique_function const&) -> unique_function& = delete;

  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  auto operator()(Args... args) const -> R {
    IOCORO_ASSERT(ptr_);
    return vtable_->invoke(ptr_, std::forward<Args>(args)...);
  }

 private:
  struct vtable {
    auto (*invoke)(void* object, Args... args) -> R;
    void (*destroy)(void* object) noexcept;
  };

  template <typename F>
  static auto invoke_impl(void* object, Args... args) -> R {
    auto& functor = *static_cast<F*>(object);
    if constexpr (std::is_void_v<R>) {
      std::invoke(functor, std::forward<Args>(args)...);
      return;
    } else {
      return std::invoke(functor, std::forward<Args>(args)...);
    }
  }

  template <typename F>
  static void destroy_impl(void* object) noexcept {
    delete static_cast<F*>(object);
  }

  template <typename F>
  static inline constexpr vtable vtable_for{
      .invoke = &invoke_impl<F>,
      .destroy = &destroy_impl<F>,
  };

  void reset() noexcept {
    if (ptr_ != nullptr) {
      vtable_->destroy(ptr_);
      ptr_ = nullptr;
      vtable_ = nullptr;
    }
  }

  void move_from(unique_function& other) noexcept {
    ptr_ = other.ptr_;
    vtable_ = other.vtable_;
    other.ptr_ = nullptr;
    other.vtable_ = nullptr;
  }

  void* ptr_{};
  vtable const* vtable_{};
};

}  // namespace iocoro::detail


