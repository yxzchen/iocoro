#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/io_context.hpp>

namespace iocoro {

/// RAII work token for an executor.
///
/// Semantics:
/// - Increments the executor's internal "outstanding work" count on construction.
/// - Decrements it on destruction (or `reset()`).
///
/// This is typically used to keep an event loop from exiting due to "no work" while some
/// external condition still requires the loop to stay alive.
template <typename Executor>
class work_guard {
 public:
  using executor_type = Executor;

  /// Acquire a unit of work for `ex`.
  explicit work_guard(executor_type const& ex) : executor_(ex), owns_(true) {
    IOCORO_ENSURE(executor_, "work_guard: requires a non-empty executor");
    executor_.add_work_guard();
  }

  work_guard(work_guard const& other) = delete;
  auto operator=(work_guard const& other) -> work_guard& = delete;

  work_guard(work_guard&& other) noexcept : executor_(other.executor_), owns_(other.owns_) {
    other.owns_ = false;
  }

  auto operator=(work_guard&& other) noexcept -> work_guard& {
    if (this != &other) {
      reset();
      executor_ = other.executor_;
      owns_ = other.owns_;
      other.owns_ = false;
    }
    return *this;
  }

  /// Releases the work token (if owned).
  ~work_guard() noexcept { reset(); }

  auto get_executor() const noexcept -> executor_type { return executor_; }

  auto owns_work() const noexcept -> bool { return owns_; }

  /// Idempotently release the work token early.
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

/// Convenience helper.
template <typename Executor>
auto make_work_guard(Executor const& ex) -> work_guard<Executor> {
  return work_guard<Executor>(ex);
}

/// Convenience helper.
inline auto make_work_guard(io_context& ctx) -> work_guard<any_io_executor> {
  return work_guard<any_io_executor>(ctx.get_executor());
}

}  // namespace iocoro
