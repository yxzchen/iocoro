#pragma once

#include <xz/io/awaitable.hpp>

#include <array>
#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace xz::io::detail {

template <typename... Ts>
struct when_all_state {
  using result_type = std::tuple<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;

  // Input awaitables
  std::tuple<awaitable<Ts>...> awaitables_;

  // Results storage - directly store results (monostate for void, T for non-void)
  result_type results_;

  // Completion tracking
  std::atomic<std::size_t> completed_count_{0};

  // Unified completion gate - ensures resume() called exactly once
  std::atomic<bool> completed_{false};

  // Exception storage - protected by atomic CAS
  std::exception_ptr exception_;
  std::atomic<bool> exception_set_{false};

  // Continuation to resume
  std::atomic<std::coroutine_handle<>> continuation_{};

  explicit when_all_state(awaitable<Ts>&&... awaitables)
      : awaitables_(std::move(awaitables)...) {}

  // Attempt to complete the operation (called by last completer or first error)
  // Returns true if this call successfully completed (became the completer)
  auto try_complete() -> bool {
    bool expected = false;
    if (completed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      // We are the completer - resume continuation if set
      auto cont = continuation_.load(std::memory_order_acquire);
      if (cont) {
        cont.resume();
      }
      return true;
    }
    return false;
  }

  // Store exception (first one wins)
  void set_exception(std::exception_ptr ex) {
    bool expected = false;
    if (exception_set_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      exception_ = ex;
    }
  }

  template <std::size_t I>
  auto make_wrapper(std::shared_ptr<when_all_state> self) -> awaitable<void> {
    using T = std::tuple_element_t<I, std::tuple<Ts...>>;

    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(std::get<I>(self->awaitables_));
        // Store monostate directly
        std::get<I>(self->results_) = std::monostate{};
      } else {
        // Await and store result directly
        std::get<I>(self->results_) = co_await std::move(std::get<I>(self->awaitables_));
      }

      // Increment completion counter
      auto count = self->completed_count_.fetch_add(1, std::memory_order_acq_rel) + 1;

      // If we're the last to complete, try to complete the operation
      if (count == sizeof...(Ts)) {
        self->try_complete();
      }
    } catch (...) {
      // Store the exception
      self->set_exception(std::current_exception());
      // Complete immediately on error
      self->try_complete();
    }
  }

  template <std::size_t... Is>
  auto make_wrappers(std::shared_ptr<when_all_state> self, std::index_sequence<Is...>)
      -> std::array<awaitable<void>, sizeof...(Ts)> {
    // Create all wrappers in an array
    return {make_wrapper<Is>(self)...};
  }

  auto get_result() -> result_type {
    // Check for exception first
    if (exception_set_.load(std::memory_order_acquire)) {
      std::rethrow_exception(exception_);
    }
    // Return results directly (no unwrapping needed)
    return std::move(results_);
  }
};

}  // namespace xz::io::detail
