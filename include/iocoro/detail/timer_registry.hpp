#pragma once

#include <iocoro/detail/reactor_types.hpp>
#include <iocoro/error.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace iocoro::detail {

enum class timer_state : std::uint8_t {
  pending,
  fired,
  cancelled,
};

class timer_registry {
 public:
  struct timer_token {
    std::uint32_t index = 0;
    std::uint64_t generation = 0;
  };

  struct cancel_result {
    reactor_op_ptr op{};
    bool cancelled = false;
  };

  auto add_timer(std::chrono::steady_clock::time_point expiry, reactor_op_ptr op) -> timer_token;
  auto cancel(timer_token tok) noexcept -> cancel_result;
  auto next_timeout() -> std::optional<std::chrono::milliseconds>;
  auto process_expired(bool stopped) -> std::size_t;
  auto empty() const -> bool;

 private:
  struct timer_node {
    std::chrono::steady_clock::time_point expiry{};
    reactor_op_ptr op{};
    std::uint64_t generation = 1;
    timer_state state{timer_state::pending};
  };

  auto push_heap(std::uint32_t index) -> void;
  auto pop_heap() -> std::uint32_t;
  auto top_index() const -> std::uint32_t;
  auto recycle_node(std::uint32_t index) -> void;

  mutable std::mutex mtx_{};
  std::vector<timer_node> nodes_{};
  std::vector<std::uint32_t> heap_{};
  std::vector<std::uint32_t> free_{};
  std::size_t active_count_ = 0;
};

inline auto timer_registry::add_timer(std::chrono::steady_clock::time_point expiry,
                                      reactor_op_ptr op) -> timer_token {
  std::scoped_lock lk{mtx_};

  std::uint32_t index = 0;
  if (!free_.empty()) {
    index = free_.back();
    free_.pop_back();
  } else {
    index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(timer_node{});
  }

  auto& node = nodes_[index];
  node.expiry = expiry;
  node.op = std::move(op);
  node.state = timer_state::pending;
  if (node.generation == 0) {
    node.generation = 1;
  }
  ++active_count_;

  push_heap(index);

  return timer_token{index, node.generation};
}

inline auto timer_registry::cancel(timer_token tok) noexcept -> cancel_result {
  std::scoped_lock lk{mtx_};
  if (tok.generation == 0 || tok.index >= nodes_.size()) {
    return {};
  }
  auto& node = nodes_[tok.index];
  if (node.generation != tok.generation) {
    return {};
  }
  if (node.state != timer_state::pending) {
    return {};
  }
  node.state = timer_state::cancelled;
  auto op = std::move(node.op);
  node.op = {};
  return cancel_result{std::move(op), true};
}

inline auto timer_registry::next_timeout() -> std::optional<std::chrono::milliseconds> {
  std::scoped_lock lk{mtx_};

  if (heap_.empty()) {
    return std::nullopt;
  }

  auto const idx = top_index();
  auto const& node = nodes_[idx];
  if (node.state == timer_state::cancelled) {
    return std::chrono::milliseconds(0);
  }

  auto const now = std::chrono::steady_clock::now();
  if (node.expiry <= now) {
    return std::chrono::milliseconds(0);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(node.expiry - now);
}

inline auto timer_registry::process_expired(bool stopped) -> std::size_t {
  std::size_t count = 0;
  struct ready_entry {
    reactor_op_ptr op{};
    bool completed = false;
  };
  std::vector<ready_entry> ready{};
  ready.reserve(8);
  auto push_ready = [&](reactor_op_ptr op, bool completed) {
    if (op) {
      ready.push_back(ready_entry{std::move(op), completed});
    }
  };

  for (;;) {
    std::unique_lock lk{mtx_};
    if (stopped || heap_.empty()) {
      break;
    }

    auto const idx = top_index();
    auto& node = nodes_[idx];

    if (node.state == timer_state::cancelled) {
      (void)pop_heap();
      auto op = std::move(node.op);
      recycle_node(idx);
      lk.unlock();
      push_ready(std::move(op), false);
      continue;
    }

    auto const now = std::chrono::steady_clock::now();
    if (node.expiry > now) {
      break;
    }

    (void)pop_heap();
    if (node.state != timer_state::pending) {
      recycle_node(idx);
      continue;
    }

    node.state = timer_state::fired;
    auto op = std::move(node.op);
    recycle_node(idx);
    lk.unlock();
    push_ready(std::move(op), true);
  }

  for (auto& entry : ready) {
    if (entry.completed) {
      entry.op->vt->on_complete(entry.op->block);
      ++count;
    } else {
      entry.op->vt->on_abort(entry.op->block, error::operation_aborted);
    }
  }

  return count;
}

inline auto timer_registry::empty() const -> bool {
  std::scoped_lock lk{mtx_};
  return active_count_ == 0;
}

inline auto timer_registry::push_heap(std::uint32_t index) -> void {
  heap_.push_back(index);
  std::push_heap(
    heap_.begin(), heap_.end(),
    [this](std::uint32_t a, std::uint32_t b) {
      return nodes_[a].expiry > nodes_[b].expiry;
    });
}

inline auto timer_registry::pop_heap() -> std::uint32_t {
  std::pop_heap(
    heap_.begin(), heap_.end(),
    [this](std::uint32_t a, std::uint32_t b) {
      return nodes_[a].expiry > nodes_[b].expiry;
    });
  auto const idx = heap_.back();
  heap_.pop_back();
  return idx;
}

inline auto timer_registry::top_index() const -> std::uint32_t {
  return heap_.front();
}

inline auto timer_registry::recycle_node(std::uint32_t index) -> void {
  auto& node = nodes_[index];
  node.op = {};
  node.state = timer_state::fired;
  ++node.generation;
  if (node.generation == 0) {
    node.generation = 1;
  }
  free_.push_back(index);
  if (active_count_ > 0) {
    --active_count_;
  }
}

}  // namespace iocoro::detail
