#pragma once

#include <iocoro/assert.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace iocoro::detail {

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L

template <typename Signature>
using unique_function = std::move_only_function<Signature>;

#else

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
    requires(!std::is_same_v<std::decay_t<F>, unique_function>) &&
            (std::is_invocable_r_v<R, F&, Args...> || std::is_invocable_r_v<R, F const&, Args...>)
  unique_function(F&& f) {
    using functor = std::decay_t<F>;
    if constexpr (fits_inline<functor>) {
      ::new (storage_ptr()) functor(std::forward<F>(f));
      ptr_ = storage_ptr();
      vtable_ = &vtable_for<functor>;
      is_inline_ = true;
    } else {
      auto* p = new functor(std::forward<F>(f));
      ptr_ = p;
      vtable_ = &vtable_for<functor>;
      is_inline_ = false;
    }
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
    void (*destroy)(void* object, bool is_inline) noexcept;
    void (*move_inline)(void* src, void* dst) noexcept;
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
  static void destroy_impl(void* object, bool is_inline) noexcept {
    auto* ptr = static_cast<F*>(object);
    if (is_inline) {
      std::destroy_at(ptr);
    } else {
      delete ptr;
    }
  }

  template <typename F>
  static void move_inline_impl(void* src, void* dst) noexcept {
    auto* src_ptr = static_cast<F*>(src);
    ::new (dst) F(std::move(*src_ptr));
    std::destroy_at(src_ptr);
  }

  static constexpr std::size_t inline_size = 3 * sizeof(void*);
  static constexpr std::size_t inline_align = alignof(std::max_align_t);
  struct alignas(inline_align) inline_storage {
    std::byte data[inline_size];
  };

  template <typename F>
  static constexpr bool fits_inline = sizeof(F) <= inline_size && alignof(F) <= inline_align &&
                                      std::is_nothrow_move_constructible_v<F>;

  template <typename F>
  static inline constexpr vtable vtable_for{
    .invoke = &invoke_impl<F>,
    .destroy = &destroy_impl<F>,
    .move_inline = &move_inline_impl<F>,
  };

  void reset() noexcept {
    if (ptr_ != nullptr) {
      vtable_->destroy(ptr_, is_inline_);
      ptr_ = nullptr;
      vtable_ = nullptr;
      is_inline_ = false;
    }
  }

  void move_from(unique_function& other) noexcept {
    if (other.ptr_ == nullptr) {
      ptr_ = nullptr;
      vtable_ = nullptr;
      is_inline_ = false;
      return;
    }
    vtable_ = other.vtable_;
    if (other.is_inline_) {
      ptr_ = storage_ptr();
      vtable_->move_inline(other.ptr_, ptr_);
      is_inline_ = true;
    } else {
      ptr_ = other.ptr_;
      is_inline_ = false;
    }
    other.ptr_ = nullptr;
    other.vtable_ = nullptr;
    other.is_inline_ = false;
  }

  auto storage_ptr() noexcept -> void* { return static_cast<void*>(storage_.data); }

  inline_storage storage_{};
  void* ptr_{};
  vtable const* vtable_{};
  bool is_inline_{false};
};

#endif

}  // namespace iocoro::detail
