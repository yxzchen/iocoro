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

struct use_awaitable_t {
  explicit use_awaitable_t() = default;
};

// Completion token instances
inline constexpr detached_t use_detached{};
inline constexpr use_awaitable_t use_awaitable{};

namespace detail {

// Move-only type erasure for callables. This avoids storing lambda closure types as subobjects
// inside externally-linked types (fixes -Wsubobject-linkage) and supports move-only captures.
template <typename>
class unique_function;

template <typename R, typename... Args>
class unique_function<R(Args...)> {
 public:
  unique_function() = default;

  template <typename F>
    requires (!std::is_same_v<std::decay_t<F>, unique_function>) &&
             std::is_invocable_r_v<R, F&, Args...>
  unique_function(F&& f)
      : ptr_(std::make_unique<model<std::decay_t<F>>>(std::forward<F>(f))) {}

  unique_function(unique_function&&) noexcept = default;
  auto operator=(unique_function&&) noexcept -> unique_function& = default;

  unique_function(unique_function const&) = delete;
  auto operator=(unique_function const&) -> unique_function& = delete;

  explicit operator bool() const noexcept { return static_cast<bool>(ptr_); }

  auto operator()(Args... args) -> R {
    return ptr_->invoke(std::forward<Args>(args)...);
  }

 private:
  struct callable_base {
    virtual ~callable_base() = default;
    virtual auto invoke(Args... args) -> R = 0;
  };

  template <typename F>
  struct model final : callable_base {
    F f_;
    explicit model(F&& f) : f_(std::move(f)) {}
    explicit model(F const& f) : f_(f) {}
    auto invoke(Args... args) -> R override { return std::invoke(f_, std::forward<Args>(args)...); }
  };

  std::unique_ptr<callable_base> ptr_;
};

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

template <typename T>
struct detached_state {
  unique_function<awaitable<T>()> factory;

  template <typename F>
    requires std::is_invocable_r_v<awaitable<T>, F&>
  explicit detached_state(F&& f) : factory(std::forward<F>(f)) {}
};

template <typename T>
struct completion_state {
  unique_function<awaitable<T>()> factory;
  unique_function<void(std::exception_ptr)> handler;

  template <typename F, typename H>
    requires std::is_invocable_r_v<awaitable<T>, F&> &&
             std::invocable<H&, std::exception_ptr>
  completion_state(F&& f, H&& h) : factory(std::forward<F>(f)), handler(std::forward<H>(h)) {}
};

template <typename T>
auto run_detached(std::shared_ptr<detached_state<T>> state) -> spawn_task {
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

template <typename T>
auto run_with_handler(std::shared_ptr<completion_state<T>> state) -> spawn_task {
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

template <typename T>
struct awaitable_state {
  unique_function<awaitable<T>()> factory;
  std::optional<T> result;
  std::exception_ptr exception;
  std::coroutine_handle<> continuation;

  template <typename F>
    requires std::is_invocable_r_v<awaitable<T>, F&>
  explicit awaitable_state(F&& f) : factory(std::forward<F>(f)) {}
};

template <>
struct awaitable_state<void> {
  unique_function<awaitable<void>()> factory;
  std::exception_ptr exception;
  std::coroutine_handle<> continuation;

  template <typename F>
    requires std::is_invocable_r_v<awaitable<void>, F&>
  explicit awaitable_state(F&& f) : factory(std::forward<F>(f)) {}
};

template <typename T>
auto run_awaitable(std::shared_ptr<awaitable_state<T>> state) -> spawn_task {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await state->factory();
    } else {
      state->result.emplace(co_await state->factory());
    }
  } catch (...) {
    state->exception = std::current_exception();
  }

  if (state->continuation) {
    defer_resume(state->continuation);
  }
}

}  // namespace detail

// co_spawn with detached completion token - callable overload
template <typename Executor, typename F>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           (!detail::is_awaitable_v<std::decay_t<F>>)
void co_spawn(Executor& ex, F&& f, detached_t) {
  using result_t = detail::awaitable_result_t<std::invoke_result_t<F>>;
  auto state = std::make_shared<detail::detached_state<result_t>>(std::forward<F>(f));

  ex.post([state]() mutable {
    detail::start_spawn_task(detail::run_detached<result_t>(state));
  });
}

// co_spawn with completion handler void(std::exception_ptr) - callable overload
template <typename Executor, typename F, typename CompletionHandler>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           (!detail::is_awaitable_v<std::decay_t<F>>) &&
           (!std::is_same_v<std::decay_t<CompletionHandler>, detached_t>) &&
           (!std::is_same_v<std::decay_t<CompletionHandler>, use_awaitable_t>) &&
           std::invocable<std::decay_t<CompletionHandler>&, std::exception_ptr>
void co_spawn(Executor& ex, F&& f, CompletionHandler&& handler) {
  using result_t = detail::awaitable_result_t<std::invoke_result_t<F>>;
  auto state = std::make_shared<detail::completion_state<result_t>>(
    std::forward<F>(f),
    std::forward<CompletionHandler>(handler)
  );

  ex.post([state]() mutable {
    detail::start_spawn_task(detail::run_with_handler<result_t>(state));
  });
}

// co_spawn with use_awaitable - callable overload
// Returns an awaitable that completes when the spawned task completes
template <typename Executor, typename F>
  requires std::is_same_v<std::decay_t<Executor>, io_context> &&
           (!detail::is_awaitable_v<std::decay_t<F>>)
auto co_spawn(Executor& ex, F&& f, use_awaitable_t) -> awaitable<detail::awaitable_result_t<std::invoke_result_t<F>>> {
  using result_t = detail::awaitable_result_t<std::invoke_result_t<F>>;
  auto state = std::make_shared<detail::awaitable_state<result_t>>(std::forward<F>(f));

  // Start the spawned task
  ex.post([state]() mutable {
    detail::start_spawn_task(detail::run_awaitable<result_t>(state));
  });

  // Return an awaitable that waits for completion
  if constexpr (std::is_void_v<result_t>) {
    struct void_awaiter {
      std::shared_ptr<detail::awaitable_state<void>> state_;

      auto await_ready() const noexcept -> bool { return false; }

      void await_suspend(std::coroutine_handle<> h) {
        state_->continuation = h;
      }

      void await_resume() {
        if (state_->exception) {
          std::rethrow_exception(state_->exception);
        }
      }
    };

    co_await void_awaiter{state};
  } else {
    struct value_awaiter {
      std::shared_ptr<detail::awaitable_state<result_t>> state_;

      auto await_ready() const noexcept -> bool { return false; }

      void await_suspend(std::coroutine_handle<> h) {
        state_->continuation = h;
      }

      auto await_resume() -> result_t {
        if (state_->exception) {
          std::rethrow_exception(state_->exception);
        }
        return std::move(*state_->result);
      }
    };

    co_return co_await value_awaiter{state};
  }
}

}  // namespace xz::io
