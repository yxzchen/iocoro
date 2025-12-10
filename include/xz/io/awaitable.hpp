#pragma once

#include <xz/io/error.hpp>
#include <xz/io/io_context.hpp>

#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>
#include <utility>
#include <variant>

namespace xz::io {

/// Base awaitable operation for async operations with error code and stop token
template <typename Result = void>
class awaitable_op {
 public:
  awaitable_op() = default;
  explicit awaitable_op(std::stop_token stop) : stop_token_(std::move(stop)) {}
  virtual ~awaitable_op() noexcept = default;

  auto await_ready() noexcept -> bool { return ready_; }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    if (stop_requested()) {
      ec_ = make_error_code(error::operation_aborted);
      ready_ = true;
      return false;
    }

    try {
      start_operation();
    } catch (const std::system_error& se) {
      complete(se.code());
    } catch (...) {
      complete(make_error_code(error::operation_failed));
    }

    if (ready_) {
      return false;
    }

    if (stop_token_ && !stop_callback_) {
      stop_callback_.emplace(*stop_token_, [this]() { complete(make_error_code(error::operation_aborted)); });
    }

    awaiting_ = h;
    return true;
  }

  auto await_resume() {
    if (ec_) throw std::system_error(ec_);
    if constexpr (!std::is_void_v<Result>) {
      return std::move(*result_);
    }
  }

 protected:
  virtual void start_operation() = 0;

  template <typename... Args>
  void complete(std::error_code ec, Args&&... args) {
    if (ready_) return;

    ec_ = ec;

    if constexpr (!std::is_void_v<Result> && sizeof...(Args) > 0) {
      result_.emplace(std::forward<Args>(args)...);
    }

    ready_ = true;
    stop_callback_.reset();

    if (awaiting_) {
      auto h = std::exchange(awaiting_, {});
      h.resume();
    }
  }

  [[nodiscard]] bool stop_requested() const noexcept { return stop_token_ && stop_token_->stop_requested(); }

  std::coroutine_handle<> awaiting_;
  std::error_code ec_{};
  bool ready_ = false;

  std::optional<std::stop_token> stop_token_;
  std::optional<std::stop_callback<std::function<void()>>> stop_callback_;

  [[no_unique_address]] std::conditional_t<std::is_void_v<Result>, std::monostate, std::optional<Result>> result_{};
};

}  // namespace xz::io
