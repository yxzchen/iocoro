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
struct when_any_container_state {
  using result_type = std::pair<std::size_t, std::conditional_t<std::is_void_v<T>, std::monostate, T>>;

  std::vector<awaitable<T>> awaitables_;
  bool done_{false};
  std::optional<result_type> result_;
  std::exception_ptr exception_;
  std::coroutine_handle<> continuation_;
  std::vector<std::optional<awaitable<void>>> wrappers_;
  std::size_t active_{0};
  io_context* ex_{nullptr};
  std::shared_ptr<when_any_container_state> keepalive_{};

  explicit when_any_container_state(std::vector<awaitable<T>>&& awaitables)
      : awaitables_(std::move(awaitables)),
        wrappers_(awaitables_.size()) {}

  auto make_wrapper(when_any_container_state* self, std::size_t index) -> awaitable<void> {
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(self->awaitables_[index]);

        // First to complete wins (single-threaded, no race)
        if (!self->done_) {
          self->done_ = true;
          self->result_.emplace(index, std::monostate{});
          if (self->continuation_) {
            defer_resume(self->continuation_);
          }
        }
      } else {
        auto result = co_await std::move(self->awaitables_[index]);

        // First to complete wins (single-threaded, no race)
        if (!self->done_) {
          self->done_ = true;
          self->result_.emplace(index, std::move(result));
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

  void start_all(std::shared_ptr<when_any_container_state> self) {
    self->active_ = self->awaitables_.size();
    self->ex_ = try_get_current_executor();
    self->keepalive_ = self;

    for (std::size_t i = 0; i < self->awaitables_.size(); ++i) {
      self->wrappers_[i].emplace(self->make_wrapper(self.get(), i));
      start_awaitable(*self->wrappers_[i]);
    }
  }

  auto get_result() -> result_type {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    return std::move(*result_);
  }
};

}  // namespace xz::io::detail

