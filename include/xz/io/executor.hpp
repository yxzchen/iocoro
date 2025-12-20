#pragma once

#include <functional>

namespace xz::io {

class io_context;

/// Executor interface for executing work on an io_context
///
/// This class provides a lightweight, type-erased interface for submitting
/// work to an execution context. It follows a simplified version of the
/// executor model from ASIO/networking TS.
class executor {
 public:
  executor() noexcept = default;
  explicit executor(io_context& ctx) noexcept;

  executor(executor const&) noexcept = default;
  auto operator=(executor const&) noexcept -> executor& = default;
  executor(executor&&) noexcept = default;
  auto operator=(executor&&) noexcept -> executor& = default;

  /// Get the associated io_context
  auto context() const noexcept -> io_context&;

  /// Execute the given function (may be inline if already in context thread)
  void execute(std::function<void()> f) const;

  /// Post the function for later execution (never inline)
  void post(std::function<void()> f) const;

  /// Dispatch the function (inline if in context thread, otherwise queued)
  void dispatch(std::function<void()> f) const;

  /// Check if currently running in the executor's thread
  auto running_in_this_thread() const noexcept -> bool;

  /// Check if the executor is valid (associated with a context)
  explicit operator bool() const noexcept { return context_ != nullptr; }

  friend auto operator==(executor const& a, executor const& b) noexcept -> bool {
    return a.context_ == b.context_;
  }

  friend auto operator!=(executor const& a, executor const& b) noexcept -> bool {
    return a.context_ != b.context_;
  }

 private:
  io_context* context_ = nullptr;
};

}  // namespace xz::io
