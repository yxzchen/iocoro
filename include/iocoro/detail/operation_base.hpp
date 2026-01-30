#pragma once

#include <memory>
#include <system_error>
#include <utility>

namespace iocoro::detail {

/// Reactor completion object (type-erased).
///
/// Semantics:
/// - Exactly one of on_complete/on_abort is called.
/// - Called at most once.
/// - Destroyed by the reactor after callback.
/// - on_complete/on_abort/destruction happen on the reactor thread.
struct reactor_op {
  void* state{};
  void (*on_complete)(void*) noexcept = nullptr;
  void (*on_abort)(void*, std::error_code) noexcept = nullptr;
  void (*destroy)(void*) noexcept = nullptr;
};

struct reactor_op_deleter {
  void operator()(reactor_op* op) const noexcept {
    if (!op) {
      return;
    }
    if (op->destroy && op->state) {
      op->destroy(op->state);
    }
    delete op;
  }
};

using reactor_op_ptr = std::unique_ptr<reactor_op, reactor_op_deleter>;

template <typename State>
inline void reactor_on_complete(void* p) noexcept {
  static_cast<State*>(p)->on_complete();
}

template <typename State>
inline void reactor_on_abort(void* p, std::error_code ec) noexcept {
  static_cast<State*>(p)->on_abort(ec);
}

template <typename State>
inline void reactor_destroy(void* p) noexcept {
  delete static_cast<State*>(p);
}

template <typename State, typename... Args>
inline auto make_reactor_op(Args&&... args) -> reactor_op_ptr {
  auto* state = new State(std::forward<Args>(args)...);
  auto* op = new reactor_op{};
  op->state = state;
  op->on_complete = &reactor_on_complete<State>;
  op->on_abort = &reactor_on_abort<State>;
  op->destroy = &reactor_destroy<State>;
  return reactor_op_ptr{op};
}

}  // namespace iocoro::detail
