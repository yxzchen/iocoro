#pragma once

#include <iocoro/detail/reactor_types.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace iocoro::detail {

struct fd_interest {
  bool want_read = false;
  bool want_write = false;
};

class fd_registry {
 public:
  struct ready_result {
    reactor_op_ptr read{};
    reactor_op_ptr write{};
    fd_interest interest{};
  };

  struct register_result {
    std::uint64_t token = 0;
    reactor_op_ptr replaced{};
    fd_interest interest{};
  };

  struct cancel_result {
    reactor_op_ptr removed{};
    fd_interest interest{};
    bool matched = false;
  };

  struct deregister_result {
    reactor_op_ptr read{};
    reactor_op_ptr write{};
    fd_interest interest{};
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
  };

  struct slot_ref {
    reactor_op_ptr* op = nullptr;
    std::uint64_t* token = nullptr;
  };

  static auto slot_for(fd_ops& ops, fd_event_kind kind) noexcept -> slot_ref {
    if (kind == fd_event_kind::read) {
      return slot_ref{&ops.read_op, &ops.read_token};
    }
    return slot_ref{&ops.write_op, &ops.write_token};
  }

  auto interest_for(fd_ops const& ops) const noexcept -> fd_interest {
    return fd_interest{ops.read_op != nullptr, ops.write_op != nullptr};
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

inline auto fd_registry::register_impl(int fd, reactor_op_ptr op, fd_event_kind kind)
  -> register_result {
  reactor_op_ptr old{};
  fd_interest interest{};
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
  auto slot = slot_for(ops, kind);
  bool const had_op = static_cast<bool>(*slot.op);
  if (op) {
    token = *slot.token = next_token_++;
  }
  if (!had_op && op) {
    ++active_count_;
    if (static_cast<std::size_t>(fd) > max_active_fd_) {
      max_active_fd_ = static_cast<std::size_t>(fd);
    }
  }
  if (had_op && !op) {
    --active_count_;
  }
  old = std::exchange(*slot.op, std::move(op));
  interest = interest_for(ops);

  return register_result{token, std::move(old), interest};
}

inline auto fd_registry::cancel(int fd, fd_event_kind kind, std::uint64_t token) noexcept
  -> cancel_result {
  reactor_op_ptr removed{};
  fd_interest interest{};
  bool matched = false;

  if (fd < 0 || static_cast<std::size_t>(fd) >= operations_.size()) {
    return cancel_result{};
  }

  auto& ops = operations_[static_cast<std::size_t>(fd)];
  auto slot = slot_for(ops, kind);
  if (*slot.op && *slot.token == token) {
    removed = std::move(*slot.op);
    *slot.token = 0;
    matched = true;
    --active_count_;
  }

  if (!matched) {
    return cancel_result{};
  }

  interest = interest_for(ops);
  if (static_cast<std::size_t>(fd) == max_active_fd_ && !ops.read_op && !ops.write_op) {
    trim_tail(static_cast<std::size_t>(fd));
  }

  return cancel_result{std::move(removed), interest, matched};
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
    ops.read_token = 0;
    ops.write_token = 0;
    if (read) {
      --active_count_;
    }
    if (write) {
      --active_count_;
    }
    if (static_cast<std::size_t>(fd) == max_active_fd_ && !ops.read_op && !ops.write_op) {
      trim_tail(static_cast<std::size_t>(fd));
    }
  }

  return deregister_result{std::move(read), std::move(write), fd_interest{}, had_any};
}

inline auto fd_registry::take_ready(int fd, bool can_read, bool can_write) -> ready_result {
  reactor_op_ptr read{};
  reactor_op_ptr write{};
  fd_interest interest{};

  if (fd < 0 || static_cast<std::size_t>(fd) >= operations_.size()) {
    return ready_result{};
  }

  auto& ops = operations_[static_cast<std::size_t>(fd)];
  if (can_read) {
    read = std::move(ops.read_op);
    ops.read_token = 0;
    if (read) {
      --active_count_;
    }
  }
  if (can_write) {
    write = std::move(ops.write_op);
    ops.write_token = 0;
    if (write) {
      --active_count_;
    }
  }

  interest = interest_for(ops);
  if (static_cast<std::size_t>(fd) == max_active_fd_ && !ops.read_op && !ops.write_op) {
    trim_tail(static_cast<std::size_t>(fd));
  }

  return ready_result{std::move(read), std::move(write), interest};
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
    if (ops.read_op || ops.write_op) {
      break;
    }
    --i;
  }
  if (i == 0 && !operations_[0].read_op && !operations_[0].write_op) {
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
    bool const had_any = static_cast<bool>(ops.read_op) || static_cast<bool>(ops.write_op);
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
  }

  operations_.clear();
  next_token_ = 1;
  active_count_ = 0;
  max_active_fd_ = 0;
  return out;
}

}  // namespace iocoro::detail
