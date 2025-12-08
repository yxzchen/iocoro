#pragma once

#include <coroutine>
#include <system_error>
#include <utility>
#include <variant>

namespace xz::io {

/// Base awaitable operation for async operations with error code
///
/// This class provides the coroutine await interface for asynchronous operations.
/// It properly handles both synchronous and asynchronous completion:
///
/// - Synchronous completion: If the operation completes immediately in start_operation(),
///   await_suspend() returns false to avoid suspension, allowing the coroutine to continue
///   without context switching.
///
/// - Asynchronous completion: If the operation needs to wait (e.g., for I/O), await_suspend()
///   returns true to suspend the coroutine. Later, when the I/O completes, complete() is called
///   which resumes the suspended coroutine.
///
/// The suspended_ flag prevents calling awaiting_.resume() during synchronous completion,
/// as the coroutine handle is not yet in a suspended state within await_suspend().
template <typename Result = void>
class awaitable_op {
 public:
  awaitable_op() = default;

  auto await_ready() const noexcept -> bool { return ready_; }

  // Returns false if operation completed synchronously (don't suspend)
  // Returns true if operation is async (do suspend)
  auto await_suspend(std::coroutine_handle<> h) -> bool {
    awaiting_ = h;
    suspended_ = false;  // Mark that we're still in start_operation()
    start_operation();
    suspended_ = true;   // Now we've returned from start_operation()
    if (ready_) {
      // Operation completed synchronously - don't suspend
      return false;
    }
    // Operation is async - suspend and wait for complete() to resume us
    return true;
  }

  auto await_resume() {
    if (ec_) throw std::system_error(ec_);
    if constexpr (!std::is_void_v<Result>) {
      return std::move(result_);
    }
  }

 protected:
  /// Derived classes must implement this to initiate the async operation
  virtual void start_operation() = 0;

  /// Complete the operation with the given result
  /// Can be called from start_operation() (synchronous) or later (asynchronous)
  /// Only resumes the coroutine if suspended_ is true (async completion)
  template <typename... Args>
  void complete(std::error_code ec, Args&&... args) {
    ec_ = ec;
    if constexpr (sizeof...(Args) > 0 && !std::is_void_v<Result>) {
      result_ = Result{std::forward<Args>(args)...};
    }
    ready_ = true;
    // Only resume if we're actually suspended (async completion)
    // For sync completion, await_suspend() will return false instead
    if (suspended_ && awaiting_) {
      awaiting_.resume();
    }
  }

  std::coroutine_handle<> awaiting_;
  std::error_code ec_;
  bool ready_ = false;
  bool suspended_ = false;

  [[no_unique_address]] std::conditional_t<std::is_void_v<Result>, std::monostate, Result> result_{};
};

}  // namespace xz::io
