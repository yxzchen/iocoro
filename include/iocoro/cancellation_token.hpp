#pragma once

#include <iocoro/detail/unique_function.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iocoro {

namespace detail {

struct cancellation_callback_node {
  std::atomic<bool> active{true};
  unique_function<void()> fn{};
};

struct cancellation_state {
  std::atomic<bool> cancelled{false};

  mutable std::mutex mtx{};
  std::uint64_t next_id{1};
  std::unordered_map<std::uint64_t, std::shared_ptr<cancellation_callback_node>> callbacks{};
};

}  // namespace detail

class cancellation_token;

/// RAII handle for a cancellation callback registration.
///
/// Destroying this object deactivates the callback so it will not be invoked by
/// a concurrent cancellation request.
class cancellation_registration {
 public:
  cancellation_registration() noexcept = default;

  cancellation_registration(cancellation_registration const&) = delete;
  auto operator=(cancellation_registration const&) -> cancellation_registration& = delete;

  cancellation_registration(cancellation_registration&& other) noexcept
      : st_(std::exchange(other.st_, {})),
        id_(std::exchange(other.id_, 0)),
        node_(std::exchange(other.node_, {})) {}

  auto operator=(cancellation_registration&& other) noexcept -> cancellation_registration& {
    if (this != &other) {
      reset();
      st_ = std::exchange(other.st_, {});
      id_ = std::exchange(other.id_, 0);
      node_ = std::exchange(other.node_, {});
    }
    return *this;
  }

  ~cancellation_registration() { reset(); }

  void reset() noexcept {
    auto st = std::exchange(st_, {});
    auto node = std::exchange(node_, {});
    auto id = std::exchange(id_, 0);

    if (node) {
      node->active.store(false, std::memory_order_release);
    }

    if (st && id != 0) {
      std::scoped_lock lk{st->mtx};
      (void)st->callbacks.erase(id);
    }
  }

 private:
  friend class cancellation_token;
  explicit cancellation_registration(std::shared_ptr<detail::cancellation_state> st,
                                     std::uint64_t id,
                                     std::shared_ptr<detail::cancellation_callback_node> node) noexcept
      : st_(std::move(st)), id_(id), node_(std::move(node)) {}

  std::shared_ptr<detail::cancellation_state> st_{};
  std::uint64_t id_{0};
  std::shared_ptr<detail::cancellation_callback_node> node_{};
};

/// Read-only view of a cancellation source.
///
/// - Cheap to copy (shared state).
/// - Thread-safe.
/// - Callbacks can be registered to integrate with I/O operations.
class cancellation_token {
 public:
  cancellation_token() noexcept = default;
  explicit cancellation_token(std::shared_ptr<detail::cancellation_state> st) noexcept
      : st_(std::move(st)) {}

  explicit operator bool() const noexcept { return static_cast<bool>(st_); }

  auto stop_requested() const noexcept -> bool {
    if (!st_) {
      return false;
    }
    return st_->cancelled.load(std::memory_order_acquire);
  }

  template <class F>
    requires std::invocable<F&>
  auto register_callback(F&& f) -> cancellation_registration {
    if (!st_) {
      return cancellation_registration{};
    }

    if (stop_requested()) {
      // Cancellation already requested: invoke immediately to minimize race windows.
      std::invoke(f);
      return cancellation_registration{};
    }

    auto node = std::make_shared<detail::cancellation_callback_node>();
    node->fn = std::forward<F>(f);

    {
      std::scoped_lock lk{st_->mtx};
      if (!st_->cancelled.load(std::memory_order_acquire)) {
        auto id = st_->next_id++;
        st_->callbacks.emplace(id, node);
        return cancellation_registration{st_, id, std::move(node)};
      }
    }

    node->fn();

    return cancellation_registration{};
  }

 private:
  std::shared_ptr<detail::cancellation_state> st_{};
};

/// Owner that can request cancellation.
class cancellation_source {
 public:
  cancellation_source() : st_(std::make_shared<detail::cancellation_state>()) {}

  auto token() const noexcept -> cancellation_token { return cancellation_token{st_}; }

  void request_cancel() noexcept {
    if (!st_) {
      return;
    }

    // Fast path: ensure idempotence.
    if (st_->cancelled.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    std::vector<std::shared_ptr<detail::cancellation_callback_node>> cbs;
    {
      std::scoped_lock lk{st_->mtx};
      cbs.reserve(st_->callbacks.size());
      for (auto& kv : st_->callbacks) {
        cbs.push_back(std::move(kv.second));
      }
      st_->callbacks.clear();
    }

    // Invoke outside the lock (callbacks may call back into library code).
    for (auto& node : cbs) {
      if (!node) {
        continue;
      }
      if (node->active.exchange(false, std::memory_order_acq_rel)) {
        node->fn();
      }
    }
  }

 private:
  std::shared_ptr<detail::cancellation_state> st_{};
};

}  // namespace iocoro


