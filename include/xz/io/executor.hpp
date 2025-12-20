#pragma once

#include <functional>

namespace xz::io {

class io_context;

namespace detail {
class io_context_impl;
}  // namespace detail

/// Executor interface for executing work on an io_context
///
/// This class provides a lightweight, type-erased interface for submitting
/// work to an execution context. It follows a simplified version of the
/// executor model from ASIO/networking TS.
class executor {
 public:
  executor() noexcept = delete;
  explicit executor(detail::io_context_impl& impl) noexcept;

  executor(executor const&) noexcept = default;
  auto operator=(executor const&) noexcept -> executor& = default;
  executor(executor&&) noexcept = default;
  auto operator=(executor&&) noexcept -> executor& = default;

  /// Execute the given function (queued for later execution, never inline)
  void execute(std::function<void()> f) const noexcept;

  /// Post the function for later execution (never inline)
  void post(std::function<void()> f) const noexcept;

  /// Dispatch the function (inline if in context thread, otherwise queued)
  void dispatch(std::function<void()> f) const;

  friend auto operator==(executor const& a, executor const& b) noexcept -> bool {
    return a.impl_ == b.impl_;
  }

  friend auto operator!=(executor const& a, executor const& b) noexcept -> bool {
    return a.impl_ != b.impl_;
  }

 private:
  detail::io_context_impl* impl_;
};

}  // namespace xz::io
