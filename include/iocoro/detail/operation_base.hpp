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
struct reactor_vtable {
  void (*on_complete)(void*) noexcept = nullptr;
  void (*on_abort)(void*, std::error_code) noexcept = nullptr;
  void (*destroy)(void*) noexcept = nullptr;
};

struct reactor_op {
  reactor_vtable const* vt{};
  void* block{};
};

struct reactor_op_deleter {
  void operator()(reactor_op* op) const noexcept {
    if (!op) {
      return;
    }
    if (op->vt && op->block) {
      op->vt->destroy(op->block);
    }
    delete op;
  }
};

using reactor_op_ptr = std::unique_ptr<reactor_op, reactor_op_deleter>;

template <typename State>
struct reactor_op_block {
  reactor_vtable const* vt;
  State state;

  template <typename... Args>
  explicit reactor_op_block(reactor_vtable const* v, Args&&... args)
      : vt(v), state(std::forward<Args>(args)...) {}
};

template <typename State>
inline void reactor_on_complete(void* p) noexcept {
  static_cast<reactor_op_block<State>*>(p)->state.on_complete();
}

template <typename State>
inline void reactor_on_abort(void* p, std::error_code ec) noexcept {
  static_cast<reactor_op_block<State>*>(p)->state.on_abort(ec);
}

template <typename State>
inline void reactor_destroy(void* p) noexcept {
  delete static_cast<reactor_op_block<State>*>(p);
}

template <typename State>
inline reactor_vtable const* reactor_vtable_for() noexcept {
  static const reactor_vtable vt{
    &reactor_on_complete<State>,
    &reactor_on_abort<State>,
    &reactor_destroy<State>,
  };
  return &vt;
}

template <typename State, typename... Args>
inline auto make_reactor_op(Args&&... args) -> reactor_op_ptr {
  auto* block = new reactor_op_block<State>{reactor_vtable_for<State>(), std::forward<Args>(args)...};
  auto* op = new reactor_op{block->vt, block};
  return reactor_op_ptr{op};
}

}  // namespace iocoro::detail
