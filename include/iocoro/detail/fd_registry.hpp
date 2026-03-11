#pragma once

#include <iocoro/detail/reactor_types.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace iocoro::detail {

class fd_registry {
 public:
  struct ready_result {
    reactor_op_ptr read{};
    reactor_op_ptr write{};
  };

  struct register_result {
    std::uint64_t token = 0;
    reactor_op_ptr replaced{};
    reactor_op_ptr ready_now{};
  };

  struct cancel_result {
    reactor_op_ptr removed{};
    bool matched = false;
  };

  struct deregister_result {
    reactor_op_ptr read{};
    reactor_op_ptr write{};
  };

  // NOTE: fd_registry is reactor-thread-only.
  // All accesses must be serialized by io_context_impl (reactor thread ownership).
  //
  // Token model:
  // - Each successful (re)registration assigns a fresh monotonically-increasing token.
  // - Cancellation/deregistration uses (fd, kind, token) matching to avoid ABA bugs where an old
  //   cancel request could accidentally cancel a newer operation on the same fd/kind.
  auto register_read(int fd, reactor_op_ptr op) -> register_result;
  auto register_write(int fd, reactor_op_ptr op) -> register_result;
  auto cancel(int fd, fd_event_kind kind, std::uint64_t token) noexcept -> cancel_result;
  auto deregister(int fd) -> deregister_result;
  void track(int fd) noexcept;

  auto take_ready(int fd, bool can_read, bool can_write) -> ready_result;

  auto empty() const -> bool;

  struct drain_all_result {
    std::vector<int> fds{};
    std::vector<reactor_op_ptr> ops{};
  };

  // Drain all registered operations (read+write) and clear the registry.
  //
  // NOTE: fd_registry is reactor-thread-only; callers must ensure serialization.
  auto drain_all() noexcept -> drain_all_result;

 private:
  struct slot {
    reactor_op_ptr op{};
    std::uint64_t token = invalid_token;
    bool ready = false;
  };

  struct fd_ops {
    slot read{};
    slot write{};
    bool tracked = false;
  };

  static auto slot_for(fd_ops& ops, fd_event_kind kind) noexcept -> slot& {
    return kind == fd_event_kind::read ? ops.read : ops.write;
  }

  auto register_impl(int fd, reactor_op_ptr op, fd_event_kind kind) -> register_result;

  // INVARIANT:
  // - `active_count_` equals the number of non-null ops across all slots (read+write).
  std::vector<fd_ops> operations_{};
  std::uint64_t next_token_ = 1;
  std::size_t active_count_ = 0;
};

inline auto fd_registry::register_read(int fd, reactor_op_ptr op) -> register_result {
  return register_impl(fd, std::move(op), fd_event_kind::read);
}

inline auto fd_registry::register_write(int fd, reactor_op_ptr op) -> register_result {
  return register_impl(fd, std::move(op), fd_event_kind::write);
}

inline auto fd_registry::register_impl(int fd, reactor_op_ptr op,
                                       fd_event_kind kind) -> register_result {
  reactor_op_ptr old{};
  reactor_op_ptr ready_now{};
  std::uint64_t token = 0;

  if (fd < 0) {
    return register_result{};
  }
  if (static_cast<std::size_t>(fd) >= operations_.size() && !op) {
    return register_result{};
  }
  if (static_cast<std::size_t>(fd) >= operations_.size()) {
    operations_.resize(static_cast<std::size_t>(fd) + 1);
  }

  auto& ops = operations_[static_cast<std::size_t>(fd)];
  ops.tracked = true;
  auto& slot = slot_for(ops, kind);

  // Edge-triggered backends can deliver readiness before the waiter is registered.
  // Latch that readiness in the slot and consume it here to complete immediately.
  if (op && slot.ready) {
    slot.ready = false;
    ready_now = std::move(op);
    return register_result{0, {}, std::move(ready_now)};
  }

  bool const had_op = static_cast<bool>(slot.op);
  if (op) {
    token = slot.token = next_token_++;
  } else {
    slot.token = invalid_token;
  }
  if (!had_op && op) {
    ++active_count_;
  }
  if (had_op && !op) {
    --active_count_;
  }
  old = std::exchange(slot.op, std::move(op));
  return register_result{token, std::move(old), {}};
}

inline auto fd_registry::cancel(int fd, fd_event_kind kind,
                                std::uint64_t token) noexcept -> cancel_result {
  reactor_op_ptr removed{};
  bool matched = false;

  if (fd < 0 || static_cast<std::size_t>(fd) >= operations_.size()) {
    return cancel_result{};
  }

  auto& ops = operations_[static_cast<std::size_t>(fd)];
  auto& slot = slot_for(ops, kind);
  if (slot.op && slot.token == token) {
    removed = std::move(slot.op);
    slot.token = invalid_token;
    matched = true;
    --active_count_;
  }

  if (!matched) {
    return cancel_result{};
  }

  return cancel_result{std::move(removed), matched};
}

inline auto fd_registry::deregister(int fd) -> deregister_result {
  reactor_op_ptr read{};
  reactor_op_ptr write{};

  if (fd >= 0 && static_cast<std::size_t>(fd) < operations_.size()) {
    auto& ops = operations_[static_cast<std::size_t>(fd)];
    read = std::move(ops.read.op);
    write = std::move(ops.write.op);
    ops.read.token = invalid_token;
    ops.write.token = invalid_token;
    ops.read.ready = false;
    ops.write.ready = false;
    ops.tracked = false;
    if (read) {
      --active_count_;
    }
    if (write) {
      --active_count_;
    }
  }

  return deregister_result{std::move(read), std::move(write)};
}

inline void fd_registry::track(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  auto idx = static_cast<std::size_t>(fd);
  if (idx >= operations_.size()) {
    operations_.resize(idx + 1);
  }
  auto& ops = operations_[idx];
  ops.tracked = true;
}

inline auto fd_registry::take_ready(int fd, bool can_read, bool can_write) -> ready_result {
  reactor_op_ptr read{};
  reactor_op_ptr write{};

  if (fd < 0) {
    return ready_result{};
  }
  if (static_cast<std::size_t>(fd) >= operations_.size()) {
    return ready_result{};
  }

  auto& ops = operations_[static_cast<std::size_t>(fd)];
  if (!ops.tracked) {
    return ready_result{};
  }
  if (can_read) {
    auto& read_slot = ops.read;
    if (read_slot.op) {
      read = std::move(read_slot.op);
      read_slot.token = invalid_token;
      --active_count_;
    } else {
      read_slot.ready = true;
    }
  }
  if (can_write) {
    auto& write_slot = ops.write;
    if (write_slot.op) {
      write = std::move(write_slot.op);
      write_slot.token = invalid_token;
      --active_count_;
    } else {
      write_slot.ready = true;
    }
  }

  return ready_result{std::move(read), std::move(write)};
}

inline auto fd_registry::empty() const -> bool {
  return active_count_ == 0;
}

inline auto fd_registry::drain_all() noexcept -> drain_all_result {
  drain_all_result out{};
  if (operations_.empty()) {
    return out;
  }

  out.fds.reserve(operations_.size());
  out.ops.reserve(active_count_);

  for (std::size_t i = 0; i < operations_.size(); ++i) {
    auto& ops = operations_[i];
    bool const had_any =
      ops.tracked || ops.read.op || ops.write.op || ops.read.ready || ops.write.ready;
    if (had_any) {
      out.fds.push_back(static_cast<int>(i));
    }
    for (slot* s : {&ops.read, &ops.write}) {
      if (s->op) {
        out.ops.push_back(std::move(s->op));
        s->token = invalid_token;
      }
      s->ready = false;
    }
  }

  operations_.clear();
  active_count_ = 0;
  return out;
}

}  // namespace iocoro::detail
