#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/detail/unique_function.hpp>

#include <concepts>
#include <memory>
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
// - operation_base / coroutine promise details
//
// Semantics (interface-level, not capability extension):
// - post(fn): enqueue fn for later execution; never assumes inline execution.
// - dispatch(fn): may execute fn inline on the calling thread when permitted by the executor.
// - All operations are noexcept: scheduling failure must be handled by the executor implementation
//   (e.g. terminate/log/drop) rather than by throwing.

namespace iocoro {

namespace detail {
struct any_executor_access;
}  // namespace detail

template <class Ex>
concept executor = requires(Ex ex, detail::unique_function<void()> fn) {
  { ex.post(std::move(fn)) } noexcept;
  { ex.dispatch(std::move(fn)) } noexcept;
  { std::as_const(ex) == std::as_const(ex) } -> std::convertible_to<bool>;
};

class any_executor {
 public:
  any_executor() = default;

  template <executor Ex>
  any_executor(Ex ex) {
    auto storage = std::make_shared<Ex>(std::move(ex));
    storage_ = storage;
    ptr_ = storage_.get();
    vtable_ = &vtable_for<Ex>;
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

  explicit operator bool() const noexcept { return ptr_ != nullptr; }

 private:
  friend struct detail::any_executor_access;

  struct vtable {
    void (*post)(void* object, detail::unique_function<void()> fn) noexcept;
    void (*dispatch)(void* object, detail::unique_function<void()> fn) noexcept;
    auto (*equals)(void const* lhs, void const* rhs) noexcept -> bool;
    auto (*target)(void const* object, std::type_info const& ti) noexcept -> void const*;
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
  static inline constexpr vtable vtable_for{
      .post = &post_impl<Ex>,
      .dispatch = &dispatch_impl<Ex>,
      .equals = &equals_impl<Ex>,
      .target = &target_impl<Ex>,
  };

  void ensure_impl() const noexcept { IOCORO_ENSURE(ptr_, "any_executor: empty"); }

  template <class T>
  auto target() const noexcept -> T const* {
    if (!ptr_) {
      return nullptr;
    }
    return static_cast<T const*>(vtable_->target(ptr_, typeid(T)));
  }

  std::shared_ptr<void> storage_{};
  void* ptr_{};
  vtable const* vtable_{};
};

}  // namespace iocoro
