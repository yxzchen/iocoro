#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace xz::io {

// Completion token tag types
struct detached_t {
  explicit detached_t() = default;
};

// Completion token instances
inline constexpr detached_t use_detached{};

namespace detail {

// Helper to check if a type is an awaitable
template <typename T>
struct is_awaitable : std::false_type {};

template <typename T>
struct is_awaitable<awaitable<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_awaitable_v = is_awaitable<std::decay_t<T>>::value;

// Helper to extract awaitable result type
template <typename T>
struct awaitable_result;

template <typename T>
struct awaitable_result<awaitable<T>> {
  using type = T;
};

template <typename T>
using awaitable_result_t = typename awaitable_result<std::decay_t<T>>::type;

// A self-destroying detached coroutine:
// - does NOT rely on xz::io::awaitable lifetime to keep the frame alive
// - destroys its own coroutine frame at final_suspend
struct spawn_task {
  struct promise_type {
    using handle_type = std::coroutine_handle<promise_type>;

    auto get_return_object() noexcept -> spawn_task {
      return spawn_task{handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
      struct final_awaiter {
        bool await_ready() noexcept { return false; }
        void await_suspend(handle_type h) noexcept { h.destroy(); }
        void await_resume() noexcept {}
      };
      return final_awaiter{};
    }

    void return_void() noexcept {}

    [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;
  handle_type h_;

  explicit spawn_task(handle_type h) noexcept : h_(h) {}

  spawn_task(spawn_task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
  auto operator=(spawn_task&& other) noexcept -> spawn_task& {
    if (this != &other) {
      if (h_) h_.destroy();
      h_ = std::exchange(other.h_, {});
    }
    return *this;
  }

  ~spawn_task() {
    if (h_) h_.destroy();
  }

  spawn_task(spawn_task const&) = delete;
  auto operator=(spawn_task const&) -> spawn_task& = delete;
};

inline void start_spawn_task(spawn_task&& t) {
  auto h = std::exchange(t.h_, {});
  if (h) {
    // Enforce "no inline resumption": schedule start on the event loop.
    defer_start(h);
  }
}

template <typename Factory>
struct detached_state {
  Factory factory;

  template <typename F>
  explicit detached_state(F&& f) : factory(std::forward<F>(f)) {}
};

template <typename Factory, typename Handler>
struct completion_state {
  Factory factory;
  Handler handler;

  template <typename F, typename H>
  completion_state(F&& f, H&& h) : factory(std::forward<F>(f)), handler(std::forward<H>(h)) {}
};

template <typename T, typename Factory>
auto run_detached(std::shared_ptr<detached_state<Factory>> state) -> spawn_task {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await state->factory();
    } else {
      [[maybe_unused]] auto result = co_await state->factory();
    }
  } catch (...) {
    // Detached mode: swallow exceptions
  }
}

template <typename T, typename Factory, typename Handler>
auto run_with_handler(std::shared_ptr<completion_state<Factory, Handler>> state) -> spawn_task {
  std::exception_ptr eptr;
  try {
    if constexpr (std::is_void_v<T>) {
      co_await state->factory();
    } else {
      [[maybe_unused]] auto result = co_await state->factory();
    }
  } catch (...) {
    eptr = std::current_exception();
  }

  try {
    std::invoke(state->handler, eptr);
  } catch (...) {
    // If the completion handler throws, fail-fast: crossing the event loop boundary is unsafe.
    std::terminate();
  }
}

}  // namespace detail

// co_spawn with detached completion token - callable overload
template <typename Executor, typename F>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           (!detail::is_awaitable_v<std::decay_t<F>>)
void co_spawn(Executor& ex, F&& f, detached_t) {
  using result_t = detail::awaitable_result_t<std::invoke_result_t<F>>;
  using factory_t = std::decay_t<F>;
  auto state = std::make_shared<detail::detached_state<factory_t>>(std::forward<F>(f));

  ex.post([state]() mutable {
    detail::start_spawn_task(detail::run_detached<result_t>(state));
  });
}

// co_spawn with completion handler void(std::exception_ptr) - callable overload
template <typename Executor, typename F, typename CompletionHandler>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           (!detail::is_awaitable_v<std::decay_t<F>>) &&
           (!std::is_same_v<std::decay_t<CompletionHandler>, detached_t>) &&
           std::invocable<std::decay_t<CompletionHandler>&, std::exception_ptr>
void co_spawn(Executor& ex, F&& f, CompletionHandler&& handler) {
  using result_t = detail::awaitable_result_t<std::invoke_result_t<F>>;
  using handler_t = std::decay_t<CompletionHandler>;
  using factory_t = std::decay_t<F>;
  auto state = std::make_shared<detail::completion_state<factory_t, handler_t>>(
    std::forward<F>(f),
    std::forward<CompletionHandler>(handler)
  );

  ex.post([state]() mutable {
    detail::start_spawn_task(detail::run_with_handler<result_t>(state));
  });
}

}  // namespace xz::io
