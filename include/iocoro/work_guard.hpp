#pragma once

#include <iocoro/assert.hpp>
#include <iocoro/executor.hpp>
#include <iocoro/io_context.hpp>

namespace iocoro {

/// RAII guard that keeps an executor's context alive
///
/// This class holds an executor and prevents its associated io_context
/// from running out of work while the guard is alive. This is useful
/// when you want to ensure the context keeps running even when there
/// are no pending operations.
template <typename Executor>
class work_guard {
 public:
  using executor_type = Executor;

  /// Construct a work guard for the given executor
  explicit work_guard(executor_type const& ex) : executor_(ex), owns_(true) {
    IOCORO_ENSURE(executor_, "work_guard: requires a non-empty executor");
    executor_.add_work_guard();
  }

  /// Construct a work guard by acquiring ownership from another executor
  work_guard(work_guard const& other) = delete;
  auto operator=(work_guard const& other) -> work_guard& = delete;

  /// Move constructor
  work_guard(work_guard&& other) noexcept : executor_(other.executor_), owns_(other.owns_) {
    other.owns_ = false;
  }

  /// Move assignment
  auto operator=(work_guard&& other) noexcept -> work_guard& {
    if (this != &other) {
      reset();
      executor_ = other.executor_;
      owns_ = other.owns_;
      other.owns_ = false;
    }
    return *this;
  }

  /// Destructor releases the work guard
  ~work_guard() { reset(); }

  /// Get the associated executor
  auto get_executor() const noexcept -> executor_type { return executor_; }

  /// Check if this guard owns the work
  auto owns_work() const noexcept -> bool { return owns_; }

  /// Reset the work guard, releasing the work count
  void reset() noexcept {
    if (owns_) {
      executor_.remove_work_guard();
      owns_ = false;
    }
  }

 private:
  executor_type executor_;
  bool owns_ = false;
};

/// Helper function to create a work guard for an executor
template <typename Executor>
auto make_work_guard(Executor const& ex) -> work_guard<Executor> {
  return work_guard<Executor>(ex);
}

/// Helper function to create a work guard for an io_context
inline auto make_work_guard(io_context& ctx) -> work_guard<executor> {
  return work_guard<executor>(ctx.get_executor());
}

}  // namespace iocoro
