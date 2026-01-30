#pragma once

#include <iocoro/detail/reactor_types.hpp>

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace iocoro::detail {

struct fd_interest {
  bool want_read = false;
  bool want_write = false;
};

class fd_registry {
 public:
  struct ready_ops {
    reactor_op_ptr read{};
    reactor_op_ptr write{};
  };

  struct ready_result {
    ready_ops ops{};
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

  auto register_read(int fd, reactor_op_ptr op) -> register_result;
  auto register_write(int fd, reactor_op_ptr op) -> register_result;
  auto cancel(int fd, fd_event_kind kind, std::uint64_t token) noexcept -> cancel_result;
  auto deregister(int fd) -> deregister_result;

  auto take_ready(int fd, bool can_read, bool can_write) -> ready_result;

  auto empty() const -> bool;

 private:
  struct fd_ops {
    reactor_op_ptr read_op;
    reactor_op_ptr write_op;
    std::uint64_t read_token = fd_event_handle::invalid_token;
    std::uint64_t write_token = fd_event_handle::invalid_token;
  };

  auto interest_for(fd_ops const& ops) const noexcept -> fd_interest {
    return fd_interest{ops.read_op != nullptr, ops.write_op != nullptr};
  }

  mutable std::mutex mtx_{};
  std::vector<fd_ops> operations_{};
  std::uint64_t next_token_ = 1;
};

inline auto fd_registry::register_read(int fd, reactor_op_ptr op) -> register_result {
  reactor_op_ptr old{};
  fd_interest interest{};
  std::uint64_t token = 0;

  {
    std::scoped_lock lk{mtx_};
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
    if (op) {
      token = ops.read_token = next_token_++;
    }
    old = std::exchange(ops.read_op, std::move(op));
    interest = interest_for(ops);
  }

  return register_result{token, std::move(old), interest};
}

inline auto fd_registry::register_write(int fd, reactor_op_ptr op) -> register_result {
  reactor_op_ptr old{};
  fd_interest interest{};
  std::uint64_t token = 0;

  {
    std::scoped_lock lk{mtx_};
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
    if (op) {
      token = ops.write_token = next_token_++;
    }
    old = std::exchange(ops.write_op, std::move(op));
    interest = interest_for(ops);
  }

  return register_result{token, std::move(old), interest};
}

inline auto fd_registry::cancel(int fd, fd_event_kind kind, std::uint64_t token) noexcept
  -> cancel_result {
  reactor_op_ptr removed{};
  fd_interest interest{};
  bool matched = false;

  {
    std::scoped_lock lk{mtx_};
    if (fd < 0 || static_cast<std::size_t>(fd) >= operations_.size()) {
      return cancel_result{};
    }

    auto& ops = operations_[static_cast<std::size_t>(fd)];
    if (kind == fd_event_kind::read) {
      if (ops.read_op && ops.read_token == token) {
        removed = std::move(ops.read_op);
        ops.read_token = 0;
        matched = true;
      }
    } else {
      if (ops.write_op && ops.write_token == token) {
        removed = std::move(ops.write_op);
        ops.write_token = 0;
        matched = true;
      }
    }

    if (!matched) {
      return cancel_result{};
    }

    interest = interest_for(ops);
  }

  return cancel_result{std::move(removed), interest, matched};
}

inline auto fd_registry::deregister(int fd) -> deregister_result {
  reactor_op_ptr read{};
  reactor_op_ptr write{};
  bool had_any = false;

  {
    std::scoped_lock lk{mtx_};
    if (fd >= 0 && static_cast<std::size_t>(fd) < operations_.size()) {
      auto& ops = operations_[static_cast<std::size_t>(fd)];
      read = std::move(ops.read_op);
      write = std::move(ops.write_op);
      had_any = static_cast<bool>(read) || static_cast<bool>(write);
      ops.read_token = 0;
      ops.write_token = 0;
    }
  }

  return deregister_result{std::move(read), std::move(write), fd_interest{}, had_any};
}

inline auto fd_registry::take_ready(int fd, bool can_read, bool can_write) -> ready_result {
  reactor_op_ptr read{};
  reactor_op_ptr write{};
  fd_interest interest{};

  {
    std::scoped_lock lk{mtx_};
    if (fd < 0 || static_cast<std::size_t>(fd) >= operations_.size()) {
      return ready_result{};
    }

    auto& ops = operations_[static_cast<std::size_t>(fd)];
    if (can_read) {
      read = std::move(ops.read_op);
      ops.read_token = 0;
    }
    if (can_write) {
      write = std::move(ops.write_op);
      ops.write_token = 0;
    }

    interest = interest_for(ops);
  }

  return ready_result{ready_ops{std::move(read), std::move(write)}, interest};
}

inline auto fd_registry::empty() const -> bool {
  std::scoped_lock lk{mtx_};
  for (auto const& ops : operations_) {
    if (ops.read_op || ops.write_op) {
      return false;
    }
  }
  return true;
}

}  // namespace iocoro::detail
