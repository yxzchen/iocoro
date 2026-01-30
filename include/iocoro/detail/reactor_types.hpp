#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

namespace iocoro::detail {

class io_context_impl;
struct timer_entry;

enum class fd_event_kind : std::uint8_t { read, write };

struct fd_event_handle {
  io_context_impl* impl = nullptr;
  int fd = -1;
  fd_event_kind kind = fd_event_kind::read;
  std::uint64_t token = 0;

  static constexpr std::uint64_t invalid_token = 0;

  auto valid() const noexcept -> bool {
    return impl != nullptr && fd >= 0 && token != invalid_token;
  }
  explicit operator bool() const noexcept { return valid(); }

  void cancel() const noexcept;

  static constexpr auto invalid_handle() { return fd_event_handle{}; }
};

struct timer_event_handle {
  io_context_impl* impl = nullptr;
  std::shared_ptr<timer_entry> entry;

  auto valid() const noexcept -> bool { return impl != nullptr && entry != nullptr; }
  explicit operator bool() const noexcept { return valid(); }

  void cancel() const noexcept;

  static auto invalid_handle() { return timer_event_handle{}; }
};

struct event_handle {
  enum class kind : std::uint8_t { none, timer, fd };
  kind type = kind::none;
  timer_event_handle timer{};
  fd_event_handle fd{};

  static auto make(timer_event_handle h) noexcept -> event_handle {
    event_handle out;
    out.type = kind::timer;
    out.timer = std::move(h);
    return out;
  }
  static auto make(fd_event_handle h) noexcept -> event_handle {
    event_handle out;
    out.type = kind::fd;
    out.fd = std::move(h);
    return out;
  }

  auto valid() const noexcept -> bool {
    if (type == kind::timer) {
      return timer.valid();
    }
    if (type == kind::fd) {
      return fd.valid();
    }
    return false;
  }
  explicit operator bool() const noexcept { return valid(); }

  void cancel() const noexcept {
    if (type == kind::timer) {
      if (timer) {
        timer.cancel();
      }
      return;
    }
    if (type == kind::fd) {
      if (fd) {
        fd.cancel();
      }
    }
  }

  auto as_timer() const noexcept -> timer_event_handle {
    if (type == kind::timer) {
      return timer;
    }
    return timer_event_handle::invalid_handle();
  }

  auto as_fd() const noexcept -> fd_event_handle {
    if (type == kind::fd) {
      return fd;
    }
    return fd_event_handle::invalid_handle();
  }
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
