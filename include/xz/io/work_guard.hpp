#pragma once

namespace xz::io {

/// Work guard to keep io_context alive
template <typename Executor>
class work_guard {
 public:
  explicit work_guard(Executor& executor) noexcept : executor_(&executor) {
    executor_->add_work_guard();
  }

  ~work_guard() noexcept {
    if (executor_) {
      executor_->remove_work_guard();
    }
  }

  work_guard(work_guard const&) = delete;
  auto operator=(work_guard const&) -> work_guard& = delete;

  work_guard(work_guard&& other) noexcept : executor_(other.executor_) {
    other.executor_ = nullptr;
  }

  auto operator=(work_guard&& other) noexcept -> work_guard& {
    if (this != &other) {
      if (executor_) {
        executor_->remove_work_guard();
      }
      executor_ = other.executor_;
      other.executor_ = nullptr;
    }
    return *this;
  }

 private:
  Executor* executor_ = nullptr;
};

}  // namespace xz::io
