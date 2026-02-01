#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/unique_function.hpp>

#include <mutex>
#include <queue>
#include <type_traits>
#include <utility>

namespace iocoro {

/// An executor adapter that provides serial (non-concurrent) execution of posted tasks.
///
/// A strand wraps an underlying executor and guarantees that tasks submitted through the
/// strand are never executed concurrently with each other, even if the underlying executor
/// is multi-threaded.
///
/// Semantics:
/// - post(fn): enqueue fn and ensure a drain is scheduled onto the underlying executor.
/// - dispatch(fn): if already executing on this strand, may run fn inline; otherwise behaves
///   like post(fn).
class strand_executor {
 public:
  strand_executor() = delete;

  explicit strand_executor(any_executor base) : state_(std::make_shared<state>(std::move(base))) {}

  template <executor Ex>
  explicit strand_executor(Ex ex) : strand_executor(any_executor{std::move(ex)}) {}

  template <class F>
    requires std::is_invocable_v<F&>
  void post(F&& f) const noexcept {
    IOCORO_ENSURE(state_, "strand_executor::post: empty state");
    IOCORO_ENSURE(state_->base, "strand_executor::post: empty base executor");

    auto fn = detail::unique_function<void()>{std::forward<F>(f)};
    bool const should_schedule = state_->enqueue(std::move(fn));

    if (should_schedule) {
      // Schedule a drain on the underlying executor.
      auto st = state_;
      state_->base.post([st]() noexcept { strand_executor::drain(std::move(st)); });
    }
  }

  template <class F>
    requires std::is_invocable_v<F&>
  void dispatch(F&& f) const noexcept {
    IOCORO_ENSURE(state_, "strand_executor::dispatch: empty state");
    IOCORO_ENSURE(state_->base, "strand_executor::dispatch: empty base executor");

    auto const cur_any = detail::get_current_executor();
    auto const* cur = cur_any
      ? detail::any_executor_access::target<strand_executor>(cur_any)
      : nullptr;
    if (cur != nullptr && (*cur == *this)) {
      f();
      return;
    }

    auto fn = detail::unique_function<void()>{std::forward<F>(f)};
    bool const should_schedule = state_->enqueue(std::move(fn));
    if (should_schedule) {
      auto st = state_;
      state_->base.dispatch([st]() noexcept { strand_executor::drain(std::move(st)); });
    }
  }

  explicit operator bool() const noexcept { return state_ != nullptr && static_cast<bool>(state_->base); }

  friend auto operator==(strand_executor const& a, strand_executor const& b) noexcept -> bool {
    return a.state_.get() == b.state_.get();
  }
  friend auto operator!=(strand_executor const& a, strand_executor const& b) noexcept -> bool {
    return !(a == b);
  }

 private:
  friend struct detail::executor_traits<strand_executor>;
  struct state;

  explicit strand_executor(std::shared_ptr<state> st) noexcept : state_(std::move(st)) {}

  struct state {
    explicit state(any_executor base_) : base(std::move(base_)) {}

    any_executor base{};
    std::mutex m{};
    std::queue<detail::unique_function<void()>> tasks{};
    bool active{false};  // true if a drain is scheduled or currently running

    auto enqueue(detail::unique_function<void()> fn) -> bool {
      std::scoped_lock lk{m};
      tasks.emplace(std::move(fn));
      if (active) {
        return false;
      }
      active = true;
      return true;
    }

    auto try_pop(detail::unique_function<void()>& out) -> bool {
      std::scoped_lock lk{m};
      if (tasks.empty()) {
        active = false;
        return false;
      }
      out = std::move(tasks.front());
      tasks.pop();
      return true;
    }
  };

  static void drain(std::shared_ptr<state> st) noexcept {
    IOCORO_ENSURE(st, "strand_executor::drain: empty state");
    IOCORO_ENSURE(st->base, "strand_executor::drain: empty base executor");

    // Make the strand visible as the current executor during the whole drain.
    strand_executor ex{st};
    detail::executor_guard g{any_executor{ex}};

    detail::unique_function<void()> fn{};
    while (st->try_pop(fn)) {
      try {
        fn();
      } catch (...) {
        // Scheduling APIs are noexcept; swallow task exceptions.
      }
      fn = {};
    }
  }

  std::shared_ptr<state> state_{};
};

/// Create a strand executor wrapping an existing any_executor.
inline auto make_strand(any_executor base) -> strand_executor { return strand_executor{std::move(base)}; }

/// Create a strand executor wrapping a concrete executor.
template <executor Ex>
inline auto make_strand(Ex ex) -> strand_executor {
  return strand_executor{any_executor{std::move(ex)}};
}

}  // namespace iocoro

namespace iocoro::detail {

template <>
struct executor_traits<strand_executor> {
  static auto capabilities(strand_executor const& ex) noexcept -> executor_capability {
    if (!ex.state_) {
      return executor_capability::none;
    }
    return ex.state_->base.capabilities();
  }

  static auto io_context(strand_executor const& ex) noexcept -> io_context_impl* {
    if (!ex.state_) {
      return nullptr;
    }
    return any_executor_access::io_context(ex.state_->base);
  }
};

}  // namespace iocoro::detail


