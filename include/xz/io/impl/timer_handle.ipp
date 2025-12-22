#pragma once

#include <xz/io/detail/timer_entry.hpp>
#include <xz/io/detail/executor_guard.hpp>
#include <xz/io/timer_handle.hpp>

#include <atomic>
#include <coroutine>
#include <memory>
#include <utility>

namespace xz::io {

namespace detail {

struct timer_wait_state final : std::enable_shared_from_this<timer_wait_state> {
  executor ex{};
  std::coroutine_handle<> h{};
  std::atomic<bool> alive{true};
  std::atomic<bool> posted{false};

  void complete() noexcept {
    if (!alive.load(std::memory_order_acquire)) {
      return;
    }

    bool expected = false;
    if (!posted.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
      return;
    }

    // timer_entry guarantees waiter callbacks are executed via executor::post,
    // so it's safe (and cheaper) to resume inline here.
    executor_guard g{ex};
    h.resume();
  }
};

struct timer_wait_awaiter final {
  std::shared_ptr<detail::timer_entry> entry{};
  std::shared_ptr<timer_wait_state> st{};

  bool await_ready() const noexcept {
    if (!entry) {
      return true;
    }
    // If already completed/cancelled, don't suspend.
    if (!entry->is_pending()) {
      return true;
    }
    // If the owning context is stopped, don't suspend (avoid hanging forever).
    if (!entry->ex || entry->ex.stopped()) {
      return true;
    }
    return false;
  }

  auto await_suspend(std::coroutine_handle<> h) noexcept -> bool {
    if (!entry) {
      return false;
    }
    if (!entry->is_pending()) {
      return false;
    }
    if (!entry->ex || entry->ex.stopped()) {
      return false;
    }

    st = std::make_shared<timer_wait_state>();
    st->ex = entry->ex;
    st->h = h;

    std::weak_ptr<timer_wait_state> w = st;
    entry->add_waiter([w]() mutable {
      if (auto s = w.lock()) {
        s->complete();
      }
    });

    return true;
  }

  void await_resume() noexcept {}

  ~timer_wait_awaiter() {
    if (st) {
      st->alive.store(false, std::memory_order_release);
    }
    if (entry) {
      // Implicit cancellation: if the awaiting coroutine goes away, the timer should not fire.
      (void)entry->cancel();
      entry->notify_completion();
    }
  }
};

}  // namespace detail

auto timer_handle::cancel() noexcept -> bool {
  if (!entry_) {
    return false;
  }
  auto const cancelled = entry_->cancel();
  if (cancelled) {
    // Ensure awaiters observing this timer are completed (via posted work).
    entry_->notify_completion();
  }
  return cancelled;
}

auto timer_handle::valid() const noexcept -> bool { return entry_ && entry_->is_pending(); }

timer_handle::operator bool() const noexcept { return entry_ != nullptr; }

timer_handle::timer_handle(std::shared_ptr<detail::timer_entry> entry) noexcept
    : entry_(std::move(entry)) {}

auto timer_handle::async_wait(use_awaitable_t) -> awaitable<void> {
  co_await detail::timer_wait_awaiter{entry_};
}

}  // namespace xz::io
