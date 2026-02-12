#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/unique_function.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace iocoro {

template <typename Executor>
class work_guard;

/// Event loop / execution context for asynchronous I/O.
///
/// Semantics:
/// - `run*()` drives completion of posted tasks, timers, and I/O readiness callbacks.
/// - At most one thread may execute `run()`, `run_one()`, or `run_for()` at a time for a given
///   `io_context` instance (single reactor thread ownership).
/// - Backend failures are treated as fatal internal errors: pending I/O and timers are aborted
///   with `error::internal_error`, and the loop transitions to the stopped state.
///
/// Threading:
/// - `post()` (via the executor) and `stop()` are safe to call from any thread.
/// - Completion callbacks run on the thread currently driving `run*()` for that `io_context`.
class io_context {
 public:
  /// Internal executor type bound to this io_context.
  class executor_type {
   public:
    executor_type() noexcept = default;
    explicit executor_type(std::shared_ptr<detail::io_context_impl> impl) noexcept
        : impl_(std::move(impl)) {}

    executor_type(executor_type const&) noexcept = default;
    auto operator=(executor_type const&) noexcept -> executor_type& = default;
    executor_type(executor_type&&) noexcept = default;
    auto operator=(executor_type&&) noexcept -> executor_type& = default;

    void post(detail::unique_function<void()> f) const noexcept {
      ensure_impl().post([ex = *this, fn = std::move(f)]() mutable {
        detail::executor_guard g{any_executor{ex}};
        fn();
      });
    }

    void dispatch(detail::unique_function<void()> f) const noexcept {
      ensure_impl().dispatch([ex = *this, fn = std::move(f)]() mutable {
        detail::executor_guard g{any_executor{ex}};
        fn();
      });
    }

    /// True if the underlying context has been stopped (or this executor is empty).
    auto stopped() const noexcept -> bool { return impl_ == nullptr || impl_->stopped(); }

    explicit operator bool() const noexcept { return impl_ != nullptr; }

    friend auto operator==(executor_type const& a, executor_type const& b) noexcept -> bool {
      return a.impl_.get() == b.impl_.get();
    }

    friend auto operator!=(executor_type const& a, executor_type const& b) noexcept -> bool {
      return !(a == b);
    }

   private:
    template <typename>
    friend class work_guard;
    friend class io_context;
    friend struct detail::executor_traits<executor_type>;

    void add_work_guard() const noexcept {
      if (impl_ != nullptr) {
        impl_->add_work_guard();
      }
    }

    void remove_work_guard() const noexcept {
      if (impl_ != nullptr) {
        impl_->remove_work_guard();
      }
    }

    auto ensure_impl() const -> detail::io_context_impl& {
      IOCORO_ENSURE(impl_, "io_context::executor_type: empty impl_");
      return *impl_;
    }

    // Shared ownership for lifetime safety.
    std::shared_ptr<detail::io_context_impl> impl_{};
  };

  io_context() : impl_(std::make_shared<detail::io_context_impl>()) {}
  ~io_context() = default;

  io_context(io_context const&) = delete;
  auto operator=(io_context const&) -> io_context& = delete;
  io_context(io_context&&) = delete;
  auto operator=(io_context&&) -> io_context& = delete;

  /// Run the event loop until `stop()` is requested or there is no work.
  /// Returns the number of completed callbacks executed.
  auto run() -> std::size_t { return impl_->run(); }

  /// Run at most one completion and return.
  /// Returns 0 if no completion was ready.
  auto run_one() -> std::size_t { return impl_->run_one(); }

  /// Run the event loop for at most `timeout`, or until stopped / out of work.
  /// Returns the number of completed callbacks executed.
  auto run_for(std::chrono::milliseconds timeout) -> std::size_t { return impl_->run_for(timeout); }

  /// Request the event loop to stop (idempotent).
  ///
  /// IMPORTANT: `stop()` does not destroy pending operations. It prevents `run*()` from
  /// making further progress until `restart()` is called.
  void stop() { impl_->stop(); }

  /// Clear the stopped state so the loop can run again.
  void restart() { impl_->restart(); }

  /// True if `stop()` has been requested.
  auto stopped() const noexcept -> bool { return impl_->stopped(); }

  /// Return an IO-capable executor associated with this context.
  ///
  /// Posting or dispatching through this executor schedules work onto this `io_context`.
  auto get_executor() noexcept -> any_io_executor { return executor_type{impl_}; }

 private:
  std::shared_ptr<detail::io_context_impl> impl_;
};

}  // namespace iocoro

namespace iocoro::detail {

template <>
struct executor_traits<iocoro::io_context::executor_type> {
  static auto capabilities(iocoro::io_context::executor_type const& ex) noexcept
    -> executor_capability {
    return ex ? executor_capability::io : executor_capability::none;
  }

  static auto io_context(iocoro::io_context::executor_type const& ex) noexcept -> io_context_impl* {
    return ex.impl_.get();
  }
};

}  // namespace iocoro::detail
