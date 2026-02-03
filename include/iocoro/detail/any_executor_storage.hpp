#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/traits/executor_traits.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace iocoro {

namespace detail {

// Shared type-erased storage used by both any_executor and any_io_executor.
class any_executor_storage {
 public:
  any_executor_storage() = default;
  ~any_executor_storage() { reset(); }

  any_executor_storage(any_executor_storage const& other) { copy_from(other); }

  auto operator=(any_executor_storage const& other) -> any_executor_storage& {
    if (this != &other) {
      reset();
      copy_from(other);
    }
    return *this;
  }

  any_executor_storage(any_executor_storage&& other) noexcept { move_from(other); }

  auto operator=(any_executor_storage&& other) noexcept -> any_executor_storage& {
    if (this != &other) {
      reset();
      move_from(other);
    }
    return *this;
  }

  template <iocoro::executor Ex>
  explicit any_executor_storage(Ex ex) {
    using executor_type = std::decay_t<Ex>;
    static_assert(std::is_move_constructible_v<executor_type>);
    if constexpr (fits_inline<executor_type>) {
      ::new (storage_ptr()) executor_type(std::move(ex));
      ptr_ = storage_ptr();
      vtable_ = &vtable_for<executor_type>;
      is_inline_ = true;
    } else {
      auto storage = std::make_shared<executor_type>(std::move(ex));
      storage_ = storage;
      ptr_ = storage_.get();
      vtable_ = &vtable_for<executor_type>;
      is_inline_ = false;
    }
  }

  friend auto operator==(any_executor_storage const& a, any_executor_storage const& b) noexcept
    -> bool {
    if (!a.ptr_ && !b.ptr_) {
      return true;
    }
    if (!a.ptr_ || !b.ptr_) {
      return false;
    }
    if (a.vtable_ != b.vtable_) {
      return false;
    }
    return a.vtable_->equals(a.ptr_, b.ptr_);
  }

  friend auto operator!=(any_executor_storage const& a, any_executor_storage const& b) noexcept
    -> bool {
    return !(a == b);
  }

  void post(unique_function<void()> fn) const noexcept {
    ensure_impl();
    vtable_->post(ptr_, std::move(fn));
  }

  void dispatch(unique_function<void()> fn) const noexcept {
    ensure_impl();
    vtable_->dispatch(ptr_, std::move(fn));
  }

  auto capabilities() const noexcept -> iocoro::executor_capability {
    if (!ptr_) {
      return executor_capability::none;
    }
    return vtable_->capabilities(ptr_);
  }

  auto io_context_ptr() const noexcept -> io_context_impl* {
    if (!ptr_) {
      return nullptr;
    }
    return vtable_->io_context(ptr_);
  }

  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  template <class T>
  auto target() const noexcept -> T const* {
    if (!ptr_) {
      return nullptr;
    }
    return static_cast<T const*>(vtable_->target(ptr_, typeid(T)));
  }

 private:
  struct vtable {
    void (*post)(void* object, unique_function<void()> fn) noexcept;
    void (*dispatch)(void* object, unique_function<void()> fn) noexcept;
    auto (*equals)(void const* lhs, void const* rhs) noexcept -> bool;
    auto (*target)(void const* object, std::type_info const& ti) noexcept -> void const*;
    auto (*capabilities)(void const* object) noexcept -> iocoro::executor_capability;
    auto (*io_context)(void const* object) noexcept -> io_context_impl*;
    void (*destroy_inline)(void* object) noexcept;
    void (*copy_inline)(void const* src, void* dst);
    void (*move_inline)(void* src, void* dst) noexcept;
  };

  template <class Ex>
  static void post_impl(void* object, unique_function<void()> fn) noexcept {
    static_cast<Ex*>(object)->post(std::move(fn));
  }

  template <class Ex>
  static void dispatch_impl(void* object, unique_function<void()> fn) noexcept {
    static_cast<Ex*>(object)->dispatch(std::move(fn));
  }

  template <class Ex>
  static auto equals_impl(void const* lhs, void const* rhs) noexcept -> bool {
    auto const* left = static_cast<Ex const*>(lhs);
    auto const* right = static_cast<Ex const*>(rhs);
    return *left == *right;
  }

  template <class Ex>
  static auto target_impl(void const* object, std::type_info const& ti) noexcept -> void const* {
    if (ti == typeid(Ex)) {
      return static_cast<Ex const*>(object);
    }
    return nullptr;
  }

  template <class Ex>
  static auto capabilities_impl(void const* object) noexcept -> iocoro::executor_capability {
    return executor_traits<Ex>::capabilities(*static_cast<Ex const*>(object));
  }

  template <class Ex>
  static auto io_context_impl_fn(void const* object) noexcept -> io_context_impl* {
    return executor_traits<Ex>::io_context(*static_cast<Ex const*>(object));
  }

  template <class Ex>
  static void destroy_inline_impl(void* object) noexcept {
    std::destroy_at(static_cast<Ex*>(object));
  }

  template <class Ex>
  static void copy_inline_impl(void const* src, void* dst) {
    ::new (dst) Ex(*static_cast<Ex const*>(src));
  }

  template <class Ex>
  static void move_inline_impl(void* src, void* dst) noexcept {
    auto* ptr = static_cast<Ex*>(src);
    ::new (dst) Ex(std::move(*ptr));
    std::destroy_at(ptr);
  }

  static constexpr std::size_t inline_size = 3 * sizeof(void*);
  static constexpr std::size_t inline_align = alignof(std::max_align_t);
  struct alignas(inline_align) inline_storage {
    std::byte data[inline_size];
  };

  template <class Ex>
  static constexpr bool fits_inline =
    sizeof(Ex) <= inline_size && alignof(Ex) <= inline_align &&
    std::is_nothrow_move_constructible_v<Ex> && std::is_nothrow_copy_constructible_v<Ex>;

  template <class Ex>
  static inline constexpr vtable vtable_for{
    .post = &post_impl<Ex>,
    .dispatch = &dispatch_impl<Ex>,
    .equals = &equals_impl<Ex>,
    .target = &target_impl<Ex>,
    .capabilities = &capabilities_impl<Ex>,
    .io_context = &io_context_impl_fn<Ex>,
    .destroy_inline = &destroy_inline_impl<Ex>,
    .copy_inline = &copy_inline_impl<Ex>,
    .move_inline = &move_inline_impl<Ex>,
  };

  void ensure_impl() const noexcept { IOCORO_ENSURE(ptr_, "any_executor_storage: empty"); }

  void reset() noexcept {
    if (ptr_ != nullptr) {
      if (is_inline_) {
        vtable_->destroy_inline(ptr_);
      } else {
        storage_.reset();
      }
      ptr_ = nullptr;
      vtable_ = nullptr;
      is_inline_ = false;
    }
  }

  void copy_from(any_executor_storage const& other) {
    if (other.ptr_ == nullptr) {
      ptr_ = nullptr;
      vtable_ = nullptr;
      is_inline_ = false;
      return;
    }
    if (other.is_inline_) {
      other.vtable_->copy_inline(other.ptr_, storage_ptr());
      ptr_ = storage_ptr();
      vtable_ = other.vtable_;
      is_inline_ = true;
    } else {
      storage_ = other.storage_;
      ptr_ = storage_.get();
      vtable_ = other.vtable_;
      is_inline_ = false;
    }
  }

  void move_from(any_executor_storage& other) noexcept {
    if (other.ptr_ == nullptr) {
      ptr_ = nullptr;
      vtable_ = nullptr;
      is_inline_ = false;
      return;
    }
    auto const* vt = other.vtable_;
    if (other.is_inline_) {
      vt->move_inline(other.ptr_, storage_ptr());
      ptr_ = storage_ptr();
      vtable_ = vt;
      is_inline_ = true;
    } else {
      storage_ = std::move(other.storage_);
      ptr_ = storage_.get();
      vtable_ = vt;
      is_inline_ = false;
    }
    other.ptr_ = nullptr;
    other.vtable_ = nullptr;
    other.is_inline_ = false;
  }

  auto storage_ptr() noexcept -> void* { return static_cast<void*>(inline_storage_.data); }

  inline_storage inline_storage_{};
  std::shared_ptr<void> storage_{};
  void* ptr_{};
  vtable const* vtable_{};
  bool is_inline_{false};
};

}  // namespace detail

}  // namespace iocoro
