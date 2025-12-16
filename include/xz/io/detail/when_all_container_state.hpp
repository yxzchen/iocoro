#pragma once

#include <xz/io/awaitable.hpp>

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace xz::io::detail {

template <typename T>
struct when_all_container_state {
  using stored_t = std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>>;
  using result_type = std::conditional_t<std::is_void_v<T>, std::vector<std::monostate>, std::vector<T>>;

  // Input awaitables
  std::vector<awaitable<T>> awaitables_;

  // Results storage
  std::vector<stored_t> results_;

  // Completion tracking (no atomic needed - single threaded event loop)
  std::size_t completed_count_{0};

  // Unified completion gate - ensures resume() called exactly once
  bool completed_{false};

  // Exception storage
  std::exception_ptr exception_;
  bool exception_set_{false};

  // Continuation to resume
  std::coroutine_handle<> continuation_;

  // Wrapper coroutines need to stay alive until completion.
  std::vector<std::optional<awaitable<void>>> wrappers_;
  std::size_t active_{0};
  io_context* ex_{nullptr};
  std::shared_ptr<when_all_container_state> keepalive_{};

  explicit when_all_container_state(std::vector<awaitable<T>>&& awaitables)
      : awaitables_(std::move(awaitables)),
        results_(awaitables_.size()),
        wrappers_(awaitables_.size()) {}

  // Attempt to complete the operation (called by last completer or first error)
  // Returns true if this call successfully completed (became the completer)
  auto try_complete() -> bool {
    if (!completed_) {
      completed_ = true;
      // Resume continuation if set
      if (continuation_) {
        defer_resume(continuation_);
      }
      return true;
    }
    return false;
  }

  // Store exception (first one wins)
  void set_exception(std::exception_ptr ex) {
    if (!exception_set_) {
      exception_set_ = true;
      exception_ = ex;
    }
  }

  auto make_wrapper(when_all_container_state* self, std::size_t index) -> awaitable<void> {
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(self->awaitables_[index]);
        self->results_[index] = std::monostate{};
      } else {
        self->results_[index].emplace(co_await std::move(self->awaitables_[index]));
      }

      // Increment completion counter
      ++self->completed_count_;

      // If we're the last to complete, try to complete the operation
      if (self->completed_count_ == self->awaitables_.size()) {
        self->try_complete();
      }
    } catch (...) {
      // Store the exception
      self->set_exception(std::current_exception());
      // Fail-fast semantics: resume the waiter as soon as the first exception happens
      self->try_complete();
    }

    // Keep the state alive until *all* wrappers finish
    if (self->active_ > 0 && --self->active_ == 0) {
      if (self->ex_) {
        self->ex_->post([keep = std::move(self->keepalive_)]() mutable { keep.reset(); });
      } else {
        self->keepalive_.reset();
      }
    }
  }

  void start_all(std::shared_ptr<when_all_container_state> self) {
    self->active_ = self->awaitables_.size();
    self->ex_ = try_get_current_executor();
    self->keepalive_ = self;

    for (std::size_t i = 0; i < self->awaitables_.size(); ++i) {
      self->wrappers_[i].emplace(self->make_wrapper(self.get(), i));
      start_awaitable(*self->wrappers_[i]);
    }
  }

  auto get_result() -> result_type {
    // Check for exception first
    if (exception_set_) {
      std::rethrow_exception(exception_);
    }

    result_type results;
    results.reserve(results_.size());

    if constexpr (std::is_void_v<T>) {
      for (auto const& r : results_) {
        results.push_back(r);
      }
    } else {
      for (auto& r : results_) {
        results.push_back(std::move(*r));
      }
    }

    return results;
  }
};

}  // namespace xz::io::detail

