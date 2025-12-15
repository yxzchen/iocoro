#pragma once

#include <xz/io/awaitable.hpp>

#include <array>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace xz::io::detail {

template <typename... Ts>
struct when_all_state {
  template <typename T>
  using stored_t = std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>>;

  using result_type = std::tuple<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;
  using storage_type = std::tuple<stored_t<Ts>...>;

  // Input awaitables
  std::tuple<awaitable<Ts>...> awaitables_;

  // Results storage. Use optionals to avoid requiring default-constructible Ts.
  storage_type results_{};

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
  std::array<std::optional<awaitable<void>>, sizeof...(Ts)> wrappers_{};
  std::size_t active_{0};
  io_context* ex_{nullptr};
  std::shared_ptr<when_all_state> keepalive_{};

  explicit when_all_state(awaitable<Ts>&&... awaitables)
      : awaitables_(std::move(awaitables)...) {}

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

  template <std::size_t I>
  auto make_wrapper(when_all_state* self) -> awaitable<void> {
    using T = std::tuple_element_t<I, std::tuple<Ts...>>;

    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(std::get<I>(self->awaitables_));
        std::get<I>(self->results_) = std::monostate{};
      } else {
        std::get<I>(self->results_).emplace(co_await std::move(std::get<I>(self->awaitables_)));
      }

      // Increment completion counter
      ++self->completed_count_;

      // If we're the last to complete, try to complete the operation
      if (self->completed_count_ == sizeof...(Ts)) {
        self->try_complete();
      }
    } catch (...) {
      // Store the exception
      self->set_exception(std::current_exception());
      // Complete immediately on error
      self->try_complete();
    }

    // Keep the state alive until *all* wrappers finish (they may continue running after the waiter resumes).
    if (self->active_ > 0 && --self->active_ == 0) {
      if (self->ex_) {
        self->ex_->post([keep = std::move(self->keepalive_)]() mutable { keep.reset(); });
      } else {
        self->keepalive_.reset();
      }
    }
  }

  template <std::size_t... Is>
  void start_all(std::shared_ptr<when_all_state> self, std::index_sequence<Is...>) {
    self->active_ = sizeof...(Ts);
    self->ex_ = try_get_current_executor();
    self->keepalive_ = self;

    ((std::get<Is>(self->wrappers_).emplace(self->template make_wrapper<Is>(self.get()))), ...);
    ((start_awaitable(*std::get<Is>(self->wrappers_))), ...);
  }

  template <std::size_t I>
  auto take_result() -> std::tuple_element_t<I, result_type> {
    using T = std::tuple_element_t<I, std::tuple<Ts...>>;
    if constexpr (std::is_void_v<T>) {
      return std::monostate{};
    } else {
      return std::move(*std::get<I>(results_));
    }
  }

  template <std::size_t... Is>
  auto make_result(std::index_sequence<Is...>) -> result_type {
    return result_type{take_result<Is>()...};
  }

  auto get_result() -> result_type {
    // Check for exception first
    if (exception_set_) {
      std::rethrow_exception(exception_);
    }
    return make_result(std::make_index_sequence<sizeof...(Ts)>{});
  }
};

}  // namespace xz::io::detail
