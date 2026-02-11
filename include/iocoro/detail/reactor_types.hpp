#pragma once

#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace iocoro::detail {

class io_context_impl;

enum class fd_event_kind : std::uint8_t { read, write };

struct event_handle {
  enum class kind : std::uint8_t { none, timer, fd };

  // Weak reference for safe cancellation. The control block is owned by
  // io_context_impl (via shared_ptr) and any executors/objects that keep it alive.
  std::weak_ptr<io_context_impl> impl{};
  kind type = kind::none;

  int fd = -1;
  fd_event_kind fd_kind = fd_event_kind::read;
  std::uint64_t token = 0;

  std::uint32_t timer_index = 0;
  std::uint64_t timer_generation = 0;

  static constexpr std::uint64_t invalid_token = 0;

  static auto make_fd(std::weak_ptr<io_context_impl> impl_, int fd_, fd_event_kind kind_,
                      std::uint64_t token_) noexcept -> event_handle {
    return event_handle{
      .impl = std::move(impl_),
      .type = kind::fd,
      .fd = fd_,
      .fd_kind = kind_,
      .token = token_,
    };
  }

  static auto make_timer(std::weak_ptr<io_context_impl> impl_, std::uint32_t index,
                         std::uint64_t generation) noexcept -> event_handle {
    return event_handle{
      .impl = std::move(impl_),
      .type = kind::timer,
      .timer_index = index,
      .timer_generation = generation,
    };
  }

  static auto invalid_handle() noexcept -> event_handle { return event_handle{}; }

  auto valid() const noexcept -> bool {
    if (impl.expired()) {
      return false;
    }
    switch (type) {
      case kind::fd:
        return fd >= 0 && token != invalid_token;
      case kind::timer:
        return timer_generation != 0;
      case kind::none:
      default:
        return false;
    }
  }
  explicit operator bool() const noexcept { return valid(); }

  void cancel() const noexcept;
};

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
  State state;

  template <typename... Args>
  explicit reactor_op_block(Args&&... args) : state(std::forward<Args>(args)...) {}
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
  auto* block = new reactor_op_block<State>{std::forward<Args>(args)...};
  auto* op = new reactor_op{reactor_vtable_for<State>(), block};
  return reactor_op_ptr{op};
}

}  // namespace iocoro::detail
