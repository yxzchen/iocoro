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

/// Base awaitable operation for async operations with error code and stop token
template <typename Result = void>
class awaitable_op {
 public:
  awaitable_op() = default;
  explicit awaitable_op(std::stop_token stop) : stop_token_(std::move(stop)) {}
  virtual ~awaitable_op() noexcept = default;

  auto await_ready() const noexcept -> bool { return ready_ || stop_requested(); }

  [[nodiscard]] auto await_suspend(std::coroutine_handle<> h) -> bool {
    awaiting_ = h;

    if (stop_requested()) {
      ec_ = make_error_code(error::operation_aborted);
      ready_ = true;
      return false;
    }

    in_await_suspend_ = true;

    if (stop_token_ && !stop_callback_) {
      stop_callback_.emplace(*stop_token_, [this]() {
        if constexpr (std::is_void_v<Result>) {
          complete(make_error_code(error::operation_aborted));
        } else {
          complete(make_error_code(error::operation_aborted), Result{});
        }
      });
    }

    start_operation();

    in_await_suspend_ = false;

    if (ready_) {
      stop_callback_.reset();
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

  [[nodiscard]] auto stop_requested() const noexcept -> bool { return stop_token_ && stop_token_->stop_requested(); }

 protected:
  virtual void start_operation() = 0;

  template <typename... Args>
  void complete(std::error_code ec, Args&&... args) {
    static_assert(std::is_void_v<Result> || sizeof...(Args) > 0, "Non-void Result requires result value in complete()");
    ec_ = ec;
    if constexpr (sizeof...(Args) > 0 && !std::is_void_v<Result>) {
      result_ = Result{std::forward<Args>(args)...};
    }
    ready_ = true;
    if (!in_await_suspend_ && awaiting_) {
      auto h = std::exchange(awaiting_, {});
      h.resume();
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
