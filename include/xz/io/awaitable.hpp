#pragma once

#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>
#include <utility>
#include <variant>

#include "error.hpp"

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
  explicit awaitable_op(std::stop_token stop) : stop_token_(std::move(stop)) {}
  virtual ~awaitable_op() = default;

  auto await_ready() const noexcept -> bool {
    // Already ready if operation completed or cancellation requested
    return ready_ || stop_requested();
  }

  // Returns false if operation completed synchronously (don't suspend)
  // Returns true if operation is async (do suspend)
  [[nodiscard]] auto await_suspend(std::coroutine_handle<> h) -> bool {
    awaiting_ = h;

    // Check if already cancelled
    if (stop_requested()) {
      ec_ = make_error_code(error::operation_aborted);
      ready_ = true;
      return false;  // Don't suspend, operation already cancelled
    }

    in_await_suspend_ = true;  // Mark that we're inside await_suspend()

    // Install stop callback before starting operation
    if (stop_token_) {
      stop_callback_.emplace(*stop_token_, [this]() {
        // Cancellation requested - complete with operation_aborted
        if constexpr (std::is_void_v<Result>) {
          complete(make_error_code(error::operation_aborted));
        } else {
          complete(make_error_code(error::operation_aborted), Result{});
        }
      });
    }

    start_operation();

    in_await_suspend_ = false;  // Now we've returned from start_operation()

    if (ready_) {
      // Operation completed synchronously - don't suspend
      stop_callback_.reset();  // Cancel the stop callback
      return false;
    }
    // Operation is async - suspend and wait for complete() to resume us
    return true;
  }

  auto await_resume() {
    // Don't reset stop_callback_ here - let destructor handle it
    // to avoid potential issues if called from within the callback
    if (ec_) throw std::system_error(ec_);
    if constexpr (!std::is_void_v<Result>) {
      return std::move(result_);
    }
  }

  /// Set stop token for this operation (must be called before co_await)
  void set_stop_token(std::stop_token stop) {
    stop_token_ = std::move(stop);
  }

  /// Check if cancellation has been requested
  [[nodiscard]] auto stop_requested() const noexcept -> bool {
    return stop_token_ && stop_token_->stop_requested();
  }

 protected:
  /// Derived classes must implement this to initiate the async operation
  virtual void start_operation() = 0;

  /// Complete the operation with the given result
  /// Can be called from start_operation() (synchronous) or later (asynchronous)
  /// Only resumes the coroutine if we've exited await_suspend()
  /// For non-void Result types, result value must always be provided (use Result{} for errors)
  template <typename... Args>
  void complete(std::error_code ec, Args&&... args) {
    static_assert(std::is_void_v<Result> || sizeof...(Args) > 0,
                  "Non-void Result requires result value in complete()");
    ec_ = ec;
    if constexpr (sizeof...(Args) > 0 && !std::is_void_v<Result>) {
      result_ = Result{std::forward<Args>(args)...};
    }
    ready_ = true;
    // Only resume if we've exited await_suspend()
    // For sync completion, await_suspend() will return false instead
    if (!in_await_suspend_ && awaiting_) {
      awaiting_.resume();
    }
  }

  std::coroutine_handle<> awaiting_;
  std::error_code ec_;
  bool ready_ = false;
  bool in_await_suspend_ = false;
  std::optional<std::stop_token> stop_token_;
  std::optional<std::stop_callback<std::function<void()>>> stop_callback_;

  [[no_unique_address]] std::conditional_t<std::is_void_v<Result>, std::monostate, Result> result_{};
};

}  // namespace xz::io
