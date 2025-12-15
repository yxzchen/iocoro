#pragma once

#include <xz/io/awaitable.hpp>

#include <array>
#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace xz::io::detail {

template <typename... Ts>
struct when_any_state {
  using result_variant_t = std::variant<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;
  using result_type = std::pair<std::size_t, result_variant_t>;

  std::tuple<awaitable<Ts>...> awaitables_;
  std::atomic<bool> done_{false};
  std::optional<result_type> result_;
  std::mutex result_mutex_;
  std::exception_ptr exception_;
  std::coroutine_handle<> continuation_;
  std::array<std::optional<awaitable<void>>, sizeof...(Ts)> wrappers_;

  explicit when_any_state(awaitable<Ts>&&... awaitables)
      : awaitables_(std::move(awaitables)...) {}

  template <std::size_t I>
  auto make_wrapper(std::shared_ptr<when_any_state> self) -> awaitable<void> {
    using T = std::tuple_element_t<I, std::tuple<Ts...>>;

    try {
      result_variant_t result_variant;

      if constexpr (std::is_void_v<T>) {
        co_await std::move(std::get<I>(self->awaitables_));
        result_variant.template emplace<I>(std::monostate{});
      } else {
        auto result = co_await std::move(std::get<I>(self->awaitables_));
        result_variant.template emplace<I>(std::move(result));
      }

      bool expected = false;
      if (self->done_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        {
          std::lock_guard lock(self->result_mutex_);
          self->result_.emplace(I, std::move(result_variant));
        }
        if (self->continuation_) {
          self->continuation_.resume();
        }
      }
    } catch (...) {
      bool expected = false;
      if (self->done_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        self->exception_ = std::current_exception();
        if (self->continuation_) {
          self->continuation_.resume();
        }
      }
    }
  }

  template <std::size_t... Is>
  void start_all(std::shared_ptr<when_any_state> self, std::index_sequence<Is...>) {
    ((std::get<Is>(wrappers_).emplace(make_wrapper<Is>(self))), ...);
    ((start_awaitable(*std::get<Is>(wrappers_))), ...);
  }

  auto get_result() -> result_type {
    if (exception_) {
      std::rethrow_exception(exception_);
    }

    std::lock_guard lock(result_mutex_);
    return std::move(*result_);
  }
};

}  // namespace xz::io::detail
