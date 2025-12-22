#pragma once

#include <chrono>
#include <functional>

namespace xz::io {

class steady_timer;
class timer_handle;

namespace detail {
class io_context_impl;
struct operation_base;
}  // namespace detail

template <typename Executor>
class work_guard;

/// Executor interface for executing work on an io_context
///
/// This class provides a lightweight, type-erased interface for submitting
/// work to an execution context. It follows a simplified version of the
/// executor model from ASIO/networking TS.
class executor {
 public:
  /// Default-constructed executor is an "empty" executor and must be assigned
  /// a valid context before use.
  executor() noexcept;
  explicit executor(detail::io_context_impl& impl) noexcept;

  executor(executor const&) noexcept = default;
  auto operator=(executor const&) noexcept -> executor& = default;
  executor(executor&&) noexcept = default;
  auto operator=(executor&&) noexcept -> executor& = default;

  /// Execute the given function (queued for later execution, never inline)
  void execute(std::function<void()> f) const;

  /// Post the function for later execution (never inline)
  void post(std::function<void()> f) const;

  /// Dispatch the function (inline if in context thread, otherwise queued)
  void dispatch(std::function<void()> f) const;

  /// Returns true if the associated context is stopped (or executor is empty).
  auto stopped() const noexcept -> bool;

  explicit operator bool() const noexcept { return impl_ != nullptr; }

  friend auto operator==(executor const& a, executor const& b) noexcept -> bool {
    return a.impl_ == b.impl_;
  }

  friend auto operator!=(executor const& a, executor const& b) noexcept -> bool {
    return a.impl_ != b.impl_;
  }

 private:
  template <typename>
  friend class work_guard;

  friend struct detail::operation_base;
  friend class steady_timer;

  void add_work_guard() const noexcept;
  void remove_work_guard() const noexcept;

  auto ensure_impl() const -> detail::io_context_impl&;

  /// Low-level timer registration primitive for steady_timer.
  /// Schedule a timer on the associated context.
  ///
  /// Notes:
  /// - Timeout is clamped to >= 0.
  /// - Callback is invoked on the context thread (via the context event loop).
  auto schedule_timer(std::chrono::milliseconds timeout, std::function<void()> callback) const
    -> timer_handle;

  // Non-owning pointer. The associated io_context_impl must outlive the executor.
  detail::io_context_impl* impl_;
};

}  // namespace xz::io
