#pragma once

#include <coroutine>
#include <system_error>
#include <utility>
#include <variant>

namespace xz::io {

/// Base awaitable operation for async operations with error code
template <typename Result = void>
class awaitable_op {
 public:
  awaitable_op() = default;

  auto await_ready() const noexcept -> bool { return ready_; }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    awaiting_ = h;
    suspended_ = false;
    start_operation();
    suspended_ = true;
    if (ready_) {
      return false;
    }
    return true;
  }

  auto await_resume() {
    if (ec_) throw std::system_error(ec_);
    if constexpr (!std::is_void_v<Result>) {
      return std::move(result_);
    }
  }

 protected:
  virtual void start_operation() = 0;

  template <typename... Args>
  void complete(std::error_code ec, Args&&... args) {
    ec_ = ec;
    if constexpr (sizeof...(Args) > 0 && !std::is_void_v<Result>) {
      result_ = Result{std::forward<Args>(args)...};
    }
    ready_ = true;
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

/// Awaitable operation with nothrow semantics (returns error_code instead of throwing)
template <typename Result = void>
class nothrow_awaitable_op {
 public:
  struct return_type {
    std::error_code ec;
    [[no_unique_address]] std::conditional_t<std::is_void_v<Result>, std::monostate, Result> value{};
  };

  nothrow_awaitable_op() = default;

  auto await_ready() const noexcept -> bool { return ready_; }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    awaiting_ = h;
    suspended_ = false;
    start_operation();
    suspended_ = true;
    if (ready_) {
      return false;
    }
    return true;
  }

  auto await_resume() noexcept {
    if constexpr (std::is_void_v<Result>) {
      return ec_;
    } else {
      return return_type{ec_, std::move(result_)};
    }
  }

 protected:
  virtual void start_operation() = 0;

  template <typename... Args>
  void complete(std::error_code ec, Args&&... args) {
    ec_ = ec;
    if constexpr (sizeof...(Args) > 0 && !std::is_void_v<Result>) {
      result_ = Result{std::forward<Args>(args)...};
    }
    ready_ = true;
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
