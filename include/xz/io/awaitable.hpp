#pragma once

#include <xz/io/error.hpp>
#include <xz/io/io_context.hpp>

#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

namespace xz::io {

/// Base awaitable operation for async operations with error code
template <typename Result = void>
class awaitable_op {
 public:
  awaitable_op() = default;
  virtual ~awaitable_op() noexcept = default;

  auto await_ready() noexcept -> bool { return ready_; }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
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

    if (awaiting_) {
      auto h = std::exchange(awaiting_, {});
      h.resume();
    }
  }

  std::coroutine_handle<> awaiting_;
  std::error_code ec_{};
  bool ready_ = false;

  [[no_unique_address]] std::conditional_t<std::is_void_v<Result>, std::monostate, std::optional<Result>> result_{};
};

}  // namespace xz::io
