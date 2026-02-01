#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/detail/unique_function.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <typeinfo>
#include <utility>

// This header provides a minimal and IO-agnostic executor abstraction.
//
// What this header IS:
// - A unified abstraction for "how to schedule a continuation onto an execution environment".
// - A semantic boundary that constrains type-erasure (`any_executor`).
//
// What this header is NOT (and must not include):
// - io_context / reactor / epoll / uring
// - timers / sockets / fd management
// - reactor_op / coroutine promise details
//
// Semantics (interface-level, not capability extension):
// - post(fn): enqueue fn for later execution; never assumes inline execution.
// - dispatch(fn): may execute fn inline on the calling thread when permitted by the executor.
// - All operations are noexcept: scheduling failure must be handled by the executor implementation
//   (e.g. terminate/log/drop) rather than by throwing.

namespace iocoro {

namespace detail {
struct any_executor_access;
class io_context_impl;

template <class Ex>
struct executor_traits;
}  // namespace detail

enum class executor_capability : std::uint8_t {
  none = 0,
  io = 1 << 0,
};

inline constexpr auto operator|(executor_capability lhs, executor_capability rhs) noexcept
  -> executor_capability {
  return static_cast<executor_capability>(
    static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

inline constexpr auto operator&(executor_capability lhs, executor_capability rhs) noexcept
  -> executor_capability {
  return static_cast<executor_capability>(
    static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

inline constexpr auto has_capability(executor_capability caps, executor_capability flag) noexcept
  -> bool {
  return (caps & flag) != executor_capability::none;
}

template <class Ex>
concept executor = requires(Ex ex, detail::unique_function<void()> fn) {
  { ex.post(std::move(fn)) } noexcept;
  { ex.dispatch(std::move(fn)) } noexcept;
  { std::as_const(ex) == std::as_const(ex) } -> std::convertible_to<bool>;
};

namespace detail {

template <class Ex>
struct executor_traits {
  static auto capabilities(Ex const&) noexcept -> executor_capability {
    return executor_capability::none;
  }

  static auto io_context(Ex const&) noexcept -> io_context_impl* { return nullptr; }
};

}  // namespace detail

class any_executor {
 public:
  any_executor() = default;
  ~any_executor() { reset(); }

  template <executor Ex>
  any_executor(Ex ex) {
    using executor_type = std::decay_t<Ex>;
    static_assert(std::is_copy_constructible_v<executor_type>);
    static_assert(std::is_nothrow_move_constructible_v<executor_type>);
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

  any_executor(any_executor const& other) { copy_from(other); }

  auto operator=(any_executor const& other) -> any_executor& {
    if (this != &other) {
      reset();
      copy_from(other);
    }
    return *this;
  }

  any_executor(any_executor&& other) noexcept { move_from(other); }

  auto operator=(any_executor&& other) noexcept -> any_executor& {
    if (this != &other) {
      reset();
      move_from(other);
    }
    return *this;
  }

  friend auto operator==(any_executor const& a, any_executor const& b) noexcept -> bool {
    if (!a.ptr_ && !b.ptr_) {
      return true;
    }
    if (!a.ptr_ || !b.ptr_) {
      return false;
    }
    if (a.vtable_ != b.vtable_) {
      return false;
    }
    // Equality is type-sensitive (same erased type) by design.
    return a.vtable_->equals(a.ptr_, b.ptr_);
  }
  friend auto operator!=(any_executor const& a, any_executor const& b) noexcept -> bool {
    return !(a == b);
  }

  void post(detail::unique_function<void()> fn) const noexcept {
    ensure_impl();
    vtable_->post(ptr_, std::move(fn));
  }

  void dispatch(detail::unique_function<void()> fn) const noexcept {
    ensure_impl();
    vtable_->dispatch(ptr_, std::move(fn));
  }

  auto capabilities() const noexcept -> executor_capability {
    if (!ptr_) {
      return executor_capability::none;
    }
    return vtable_->capabilities(ptr_);
  }

  auto supports_io() const noexcept -> bool {
    return has_capability(capabilities(), executor_capability::io);
  }

  auto io_context_ptr() const noexcept -> detail::io_context_impl* {
    if (!ptr_) {
      return nullptr;
    }
    return vtable_->io_context(ptr_);
  }

  explicit operator bool() const noexcept { return ptr_ != nullptr; }

 private:
  friend struct detail::any_executor_access;

  struct vtable {
    void (*post)(void* object, detail::unique_function<void()> fn) noexcept;
    void (*dispatch)(void* object, detail::unique_function<void()> fn) noexcept;
    auto (*equals)(void const* lhs, void const* rhs) noexcept -> bool;
    auto (*target)(void const* object, std::type_info const& ti) noexcept -> void const*;
    auto (*capabilities)(void const* object) noexcept -> executor_capability;
    auto (*io_context)(void const* object) noexcept -> detail::io_context_impl*;
    void (*destroy_inline)(void* object) noexcept;
    void (*copy_inline)(void const* src, void* dst);
    void (*move_inline)(void* src, void* dst) noexcept;
  };

  template <class Ex>
  static void post_impl(void* object, detail::unique_function<void()> fn) noexcept {
    static_cast<Ex*>(object)->post(std::move(fn));
  }

  template <class Ex>
  static void dispatch_impl(void* object, detail::unique_function<void()> fn) noexcept {
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
  static auto capabilities_impl(void const* object) noexcept -> executor_capability {
    return detail::executor_traits<Ex>::capabilities(*static_cast<Ex const*>(object));
  }

  template <class Ex>
  static auto io_context_impl(void const* object) noexcept -> detail::io_context_impl* {
    return detail::executor_traits<Ex>::io_context(*static_cast<Ex const*>(object));
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

  // Inline storage is intentionally small to avoid ABI bloat.
  // Larger or more strictly-aligned executors fall back to heap storage.
  static constexpr std::size_t inline_size = 3 * sizeof(void*);
  static constexpr std::size_t inline_align = alignof(std::max_align_t);
  using inline_storage = alignas(inline_align) std::byte[inline_size];

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
      .io_context = &io_context_impl<Ex>,
      .destroy_inline = &destroy_inline_impl<Ex>,
      .copy_inline = &copy_inline_impl<Ex>,
      .move_inline = &move_inline_impl<Ex>,
  };

  void ensure_impl() const noexcept { IOCORO_ENSURE(ptr_, "any_executor: empty"); }

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

  void copy_from(any_executor const& other) {
    if (other.ptr_ == nullptr) {
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

  void move_from(any_executor& other) noexcept {
    if (other.ptr_ == nullptr) {
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

  auto storage_ptr() noexcept -> void* { return static_cast<void*>(inline_storage_); }

  template <class T>
  auto target() const noexcept -> T const* {
    if (!ptr_) {
      return nullptr;
    }
    return static_cast<T const*>(vtable_->target(ptr_, typeid(T)));
  }

  inline_storage inline_storage_{};
  std::shared_ptr<void> storage_{};
  void* ptr_{};
  vtable const* vtable_{};
  bool is_inline_{false};
};

}  // namespace iocoro
