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
#include <variant>

namespace xz::io::detail {

template <typename... Ts>
struct when_any_state {
  using result_variant_t = std::variant<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;
  using result_type = std::pair<std::size_t, result_variant_t>;

  std::tuple<awaitable<Ts>...> awaitables_;
  bool done_{false};
  std::optional<result_type> result_;
  std::exception_ptr exception_;
  std::coroutine_handle<> continuation_;
  std::array<std::optional<awaitable<void>>, sizeof...(Ts)> wrappers_;
  std::size_t active_{0};
  io_context* ex_{nullptr};
  std::shared_ptr<when_any_state> keepalive_{};

  explicit when_any_state(awaitable<Ts>&&... awaitables)
      : awaitables_(std::move(awaitables)...) {}

  template <std::size_t I>
  auto make_wrapper(when_any_state* self) -> awaitable<void> {
    using T = std::tuple_element_t<I, std::tuple<Ts...>>;

    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(std::get<I>(self->awaitables_));
        result_variant_t result_variant{std::in_place_index<I>, std::monostate{}};

        // First to complete wins (single-threaded, no race)
        if (!self->done_) {
          self->done_ = true;
          self->result_.emplace(I, std::move(result_variant));
          if (self->continuation_) {
            defer_resume(self->continuation_);
          }
        }
      } else {
        auto result = co_await std::move(std::get<I>(self->awaitables_));
        result_variant_t result_variant{std::in_place_index<I>, std::move(result)};

        // First to complete wins (single-threaded, no race)
        if (!self->done_) {
          self->done_ = true;
          self->result_.emplace(I, std::move(result_variant));
          if (self->continuation_) {
            defer_resume(self->continuation_);
          }
        }
      }
    } catch (...) {
      // First to complete wins (single-threaded, no race)
      if (!self->done_) {
        self->done_ = true;
        self->exception_ = std::current_exception();
        if (self->continuation_) {
          defer_resume(self->continuation_);
        }
      }
    }

    // Keep the state alive until *all* wrappers finish, even if when_any already resumed its waiter.
    if (self->active_ > 0 && --self->active_ == 0) {
      if (self->ex_) {
        self->ex_->post([keep = std::move(self->keepalive_)]() mutable { keep.reset(); });
      } else {
        self->keepalive_.reset();
      }
    }
  }

  template <std::size_t... Is>
  void start_all(std::shared_ptr<when_any_state> self, std::index_sequence<Is...>) {
    self->active_ = sizeof...(Ts);
    self->ex_ = try_get_current_executor();
    self->keepalive_ = self;

    ((std::get<Is>(self->wrappers_).emplace(self->template make_wrapper<Is>(self.get()))), ...);
    ((start_awaitable(*std::get<Is>(self->wrappers_))), ...);
  }

  auto get_result() -> result_type {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    return std::move(*result_);
  }
};

}  // namespace xz::io::detail
