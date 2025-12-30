#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/detail/unique_function.hpp>

#include <concepts>
#include <memory>
#include <typeinfo>
#include <utility>
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
};

class any_executor {
 public:
  any_executor() = default;

  template <executor Ex>
  explicit any_executor(Ex ex) : impl_(std::make_shared<model<Ex>>(std::move(ex))) {}

  void post(detail::unique_function<void()> fn) const noexcept {
    ensure_impl()->post(std::move(fn));
  }

  void dispatch(detail::unique_function<void()> fn) const noexcept {
    ensure_impl()->dispatch(std::move(fn));
  }

  explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

 private:
  friend struct detail::any_executor_access;

  struct concept_base {
    virtual ~concept_base() = default;
    virtual void post(detail::unique_function<void()>) noexcept = 0;
    virtual void dispatch(detail::unique_function<void()>) noexcept = 0;
    virtual auto target(std::type_info const& ti) const noexcept -> void const* = 0;
  };

  template <class Ex>
  struct model final : concept_base {
    explicit model(Ex ex) : ex_(std::move(ex)) {}

    void post(detail::unique_function<void()> fn) noexcept override { ex_.post(std::move(fn)); }

    void dispatch(detail::unique_function<void()> fn) noexcept override {
      ex_.dispatch(std::move(fn));
    }

    auto target(std::type_info const& ti) const noexcept -> void const* override {
      if (ti == typeid(Ex)) {
        return std::addressof(ex_);
      }
      return nullptr;
    }

    Ex ex_;
  };

  inline auto ensure_impl() const -> std::shared_ptr<concept_base> {
    IOCORO_ENSURE(impl_, "any_executor: empty impl_");
    return impl_;
  }

  template <class T>
  auto target() const noexcept -> T const* {
    if (!impl_) {
      return nullptr;
    }
    return static_cast<T const*>(impl_->target(typeid(T)));
  }

  std::shared_ptr<concept_base> impl_{};
};

}  // namespace iocoro
