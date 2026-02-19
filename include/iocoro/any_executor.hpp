#pragma once

#include <iocoro/detail/any_executor_storage.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/traits/executor_traits.hpp>

#include <concepts>
#include <type_traits>
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
// - Scheduling behavior is executor-defined (including failure handling policy).

namespace iocoro {

class any_io_executor;

namespace detail {
struct any_executor_access;
}  // namespace detail

class any_executor {
 public:
  any_executor() = default;

  // Construct from a concrete executor (type-erased).
  template <executor Ex>
    requires(!std::same_as<std::decay_t<Ex>, any_executor>) &&
            (!std::same_as<std::decay_t<Ex>, any_io_executor>)
  any_executor(Ex ex) : storage_(std::move(ex)) {}

  // Defined in any_io_executor.hpp to avoid include cycles.
  any_executor(any_io_executor const& ex) noexcept;

  friend auto operator==(any_executor const& a, any_executor const& b) noexcept -> bool {
    return a.storage_ == b.storage_;
  }
  friend auto operator!=(any_executor const& a, any_executor const& b) noexcept -> bool {
    return !(a == b);
  }

  void post(detail::unique_function<void()> fn) const {
    if (!storage_) {
      return;
    }
    storage_.post(std::move(fn));
  }
  void dispatch(detail::unique_function<void()> fn) const {
    if (!storage_) {
      return;
    }
    storage_.dispatch(std::move(fn));
  }

  auto capabilities() const noexcept -> executor_capability { return storage_.capabilities(); }
  auto supports_io() const noexcept -> bool {
    return has_capability(capabilities(), executor_capability::io);
  }
  auto io_context_ptr() const noexcept -> detail::io_context_impl* {
    return storage_.io_context_ptr();
  }

  explicit operator bool() const noexcept { return static_cast<bool>(storage_); }

 private:
  friend struct detail::any_executor_access;
  friend class any_io_executor;

  struct storage_tag {};
  explicit any_executor(storage_tag, detail::any_executor_storage storage) noexcept
      : storage_(std::move(storage)) {}
  template <class T>
  auto target() const noexcept -> T const* {
    return storage_.target<T>();
  }

  detail::any_executor_storage storage_{};
};

}  // namespace iocoro
