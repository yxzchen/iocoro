#pragma once

#include <functional>

namespace iocoro {

class steady_timer;

namespace detail {
class io_context_impl;
struct operation_base;
namespace socket {
class socket_impl_base;
}  // namespace socket
}  // namespace detail

template <typename Executor>
class work_guard;

/// Executor interface for executing work on an io_context
class io_executor {
 public:
  /// Default-constructed io_executor is an "empty" io_executor and must be assigned
  /// a valid context before use.
  io_executor() noexcept;
  explicit io_executor(detail::io_context_impl& impl) noexcept;

  io_executor(io_executor const&) noexcept = default;
  auto operator=(io_executor const&) noexcept -> io_executor& = default;
  io_executor(io_executor&&) noexcept = default;
  auto operator=(io_executor&&) noexcept -> io_executor& = default;

  /// Execute the given function (queued for later execution, never inline)
  void execute(std::function<void()> f) const;

  /// Post the function for later execution (never inline)
  void post(std::function<void()> f) const;

  /// Dispatch the function (inline if in context thread, otherwise queued)
  void dispatch(std::function<void()> f) const;

  /// Returns true if the associated context is stopped (or io_executor is empty).
  auto stopped() const noexcept -> bool;

  explicit operator bool() const noexcept { return impl_ != nullptr; }

  friend auto operator==(io_executor const& a, io_executor const& b) noexcept -> bool {
    return a.impl_ == b.impl_;
  }

  friend auto operator!=(io_executor const& a, io_executor const& b) noexcept -> bool {
    return a.impl_ != b.impl_;
  }

 private:
  template <typename>
  friend class work_guard;

  friend class steady_timer;
  friend struct detail::operation_base;
  friend class detail::socket::socket_impl_base;

  void add_work_guard() const noexcept;
  void remove_work_guard() const noexcept;

  auto ensure_impl() const -> detail::io_context_impl&;

  // Non-owning pointer. The associated io_context_impl must outlive the io_executor.
  detail::io_context_impl* impl_;
};

}  // namespace iocoro
