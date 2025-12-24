#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace iocoro {

/// Exception thrown by sync_wait_for when the timeout expires.
class sync_wait_timeout : public std::runtime_error {
 public:
  explicit sync_wait_timeout(char const* msg) : std::runtime_error(msg) {}
};

namespace detail {

template <typename T>
struct sync_wait_state {
  bool done{false};
  std::exception_ptr ep{};
  std::optional<T> value{};
};

template <>
struct sync_wait_state<void> {
  bool done{false};
  std::exception_ptr ep{};
};

template <typename T>
inline void set_from_expected(sync_wait_state<T>& st, expected<T, std::exception_ptr>&& r) {
  st.done = true;
  if (!r) {
    st.ep = std::move(r).error();
    return;
  }
  st.value.emplace(std::move(*r));
}

inline void set_from_expected(sync_wait_state<void>& st,
                              expected<void, std::exception_ptr>&& r) noexcept {
  st.done = true;
  if (!r) st.ep = std::move(r).error();
}

template <typename T>
inline auto take_or_throw(sync_wait_state<T>& st) -> T {
  if (st.ep) std::rethrow_exception(st.ep);
  if constexpr (std::is_void_v<T>) {
    return;
  } else {
    if (!st.value.has_value()) {
      throw std::logic_error("sync_wait: missing value");
    }
    return std::move(*st.value);
  }
}

}  // namespace detail

/// Drive `ctx` until the awaitable completes; return its value or rethrow its exception.
///
/// Test utility (not part of the library API).
template <typename T>
auto sync_wait(io_context& ctx, awaitable<T> a) -> T {
  auto ex = ctx.get_executor();
  ctx.restart();

  detail::sync_wait_state<T> st{};
  co_spawn(ex, std::move(a), [&](expected<T, std::exception_ptr> r) mutable {
    detail::set_from_expected<T>(st, std::move(r));
  });

  // Drive the event loop until completion. We intentionally do NOT call ctx.stop()
  // from the completion callback: detached co_spawn posts coroutine destruction to
  // the executor at final_suspend, and stopping the context early can prevent that
  // destroy task from running (LSan/ASan leak).
  while (!st.done) {
    if (ctx.run_one() == 0) throw std::logic_error("sync_wait: no work before completion");
  }

  // Drain any remaining posted work (notably coroutine frame destruction).
  while (ctx.run_one() != 0) {
  }

  if (!st.done) throw std::logic_error("sync_wait: io_context stopped before completion");
  return detail::take_or_throw<T>(st);
}

/// Drive `ctx` for at most `timeout` until the awaitable completes; return its value or
/// rethrow its exception. Throws sync_wait_timeout on timeout.
///
/// Test utility (not part of the library API).
template <typename Rep, typename Period, typename T>
auto sync_wait_for(io_context& ctx, std::chrono::duration<Rep, Period> timeout, awaitable<T> a)
  -> T {
  auto ex = ctx.get_executor();
  ctx.restart();

  detail::sync_wait_state<T> st{};
  co_spawn(ex, std::move(a), [&](expected<T, std::exception_ptr> r) mutable {
    detail::set_from_expected<T>(st, std::move(r));
  });

  using clock = std::chrono::steady_clock;
  auto const deadline = clock::now() + timeout;

  while (!st.done) {
    auto const now = clock::now();
    if (now >= deadline) break;

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining <= std::chrono::milliseconds{0}) remaining = std::chrono::milliseconds{1};

    (void)ctx.run_for(remaining);
  }

  if (!st.done) {
    throw sync_wait_timeout("sync_wait_for: timeout");
  }

  // Drain any remaining posted work (notably coroutine frame destruction).
  while (ctx.run_one() != 0) {
  }

  return detail::take_or_throw<T>(st);
}

}  // namespace iocoro
