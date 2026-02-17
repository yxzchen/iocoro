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
    bool had_any = false;
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
  struct fd_ops {
    reactor_op_ptr read_op;
    reactor_op_ptr write_op;
    std::uint64_t read_token = invalid_token;
    std::uint64_t write_token = invalid_token;
    bool read_ready = false;
    bool write_ready = false;
    bool tracked = false;
  };

  struct slot_ref {
    reactor_op_ptr* op = nullptr;
    std::uint64_t* token = nullptr;
    bool* ready = nullptr;
  };

  static auto slot_for(fd_ops& ops, fd_event_kind kind) noexcept -> slot_ref {
    if (kind == fd_event_kind::read) {
      return slot_ref{&ops.read_op, &ops.read_token, &ops.read_ready};
    }
    return slot_ref{&ops.write_op, &ops.write_token, &ops.write_ready};
  }

  static auto has_slot_state(fd_ops const& ops) noexcept -> bool {
    return ops.tracked || ops.read_op || ops.write_op || ops.read_ready || ops.write_ready;
  }

  auto register_impl(int fd, reactor_op_ptr op, fd_event_kind kind) -> register_result;
  void trim_tail(std::size_t fd_index);

  // INVARIANT:
  // - `active_count_` equals the number of non-null ops across all slots (read+write).
  // - `max_active_fd_` tracks the highest fd index that may still contain an op; used only to
  //   bound `operations_` size (memory trimming), not for correctness of matching.
  std::vector<fd_ops> operations_{};
  std::uint64_t next_token_ = 1;
  std::size_t active_count_ = 0;
  std::size_t max_active_fd_ = 0;
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
  auto slot = slot_for(ops, kind);

  // Edge-triggered backends can deliver readiness before the waiter is registered.
  // Latch that readiness in the slot and consume it here to complete immediately.
  if (op && *slot.ready) {
    *slot.ready = false;
    ready_now = std::move(op);
    return register_result{0, {}, std::move(ready_now)};
  }

  bool const had_op = static_cast<bool>(*slot.op);
  if (op) {
    token = *slot.token = next_token_++;
  } else {
    *slot.token = invalid_token;
  }
  if (!had_op && op) {
    ++active_count_;
  }
  if (had_op && !op) {
    --active_count_;
  }
  old = std::exchange(*slot.op, std::move(op));
  if (has_slot_state(ops) && static_cast<std::size_t>(fd) > max_active_fd_) {
    max_active_fd_ = static_cast<std::size_t>(fd);
  }
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
  auto slot = slot_for(ops, kind);
  if (*slot.op && *slot.token == token) {
    removed = std::move(*slot.op);
    *slot.token = invalid_token;
    matched = true;
    --active_count_;
  }

  if (!matched) {
    return cancel_result{};
  }

  if (static_cast<std::size_t>(fd) == max_active_fd_ && !has_slot_state(ops)) {
    trim_tail(static_cast<std::size_t>(fd));
  }

  return cancel_result{std::move(removed), matched};
}

inline auto fd_registry::deregister(int fd) -> deregister_result {
  reactor_op_ptr read{};
  reactor_op_ptr write{};
  bool had_any = false;

  if (fd >= 0 && static_cast<std::size_t>(fd) < operations_.size()) {
    auto& ops = operations_[static_cast<std::size_t>(fd)];
    read = std::move(ops.read_op);
    write = std::move(ops.write_op);
    had_any = static_cast<bool>(read) || static_cast<bool>(write);
    ops.read_token = invalid_token;
    ops.write_token = invalid_token;
    ops.read_ready = false;
    ops.write_ready = false;
    ops.tracked = false;
    if (read) {
      --active_count_;
    }
    if (write) {
      --active_count_;
    }
    if (static_cast<std::size_t>(fd) == max_active_fd_ && !has_slot_state(ops)) {
      trim_tail(static_cast<std::size_t>(fd));
    }
  }

  return deregister_result{std::move(read), std::move(write), had_any};
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
  if (idx > max_active_fd_) {
    max_active_fd_ = idx;
  }
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
    if (ops.read_op) {
      read = std::move(ops.read_op);
      ops.read_token = 0;
      --active_count_;
    } else {
      ops.read_ready = true;
    }
  }
  if (can_write) {
    if (ops.write_op) {
      write = std::move(ops.write_op);
      ops.write_token = 0;
      --active_count_;
    } else {
      ops.write_ready = true;
    }
  }

  if (has_slot_state(ops) && static_cast<std::size_t>(fd) > max_active_fd_) {
    max_active_fd_ = static_cast<std::size_t>(fd);
  }

  if (static_cast<std::size_t>(fd) == max_active_fd_ && !has_slot_state(ops)) {
    trim_tail(static_cast<std::size_t>(fd));
  }

  return ready_result{std::move(read), std::move(write)};
}

inline auto fd_registry::empty() const -> bool {
  return active_count_ == 0;
}

inline void fd_registry::trim_tail(std::size_t fd_index) {
  if (operations_.empty()) {
    max_active_fd_ = 0;
    return;
  }
  std::size_t i = fd_index;
  while (i > 0) {
    auto const& ops = operations_[i];
    if (has_slot_state(ops)) {
      break;
    }
    --i;
  }
  if (i == 0 && !has_slot_state(operations_[0])) {
    operations_.clear();
    max_active_fd_ = 0;
    return;
  }
  max_active_fd_ = i;
  if (operations_.size() > max_active_fd_ + 1) {
    operations_.resize(max_active_fd_ + 1);
  }
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
    bool const had_any = has_slot_state(ops);
    if (had_any) {
      out.fds.push_back(static_cast<int>(i));
    }
    if (ops.read_op) {
      out.ops.push_back(std::move(ops.read_op));
      ops.read_token = invalid_token;
    }
    if (ops.write_op) {
      out.ops.push_back(std::move(ops.write_op));
      ops.write_token = invalid_token;
    }
    ops.read_ready = false;
    ops.write_ready = false;
  }

  operations_.clear();
  active_count_ = 0;
  max_active_fd_ = 0;
  return out;
}

}  // namespace iocoro::detail
