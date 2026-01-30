#pragma once

#include <iocoro/detail/reactor_types.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

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
  std::unordered_map<int, fd_ops> operations_{};
  std::uint64_t next_token_ = 1;
};

inline auto fd_registry::register_read(int fd, reactor_op_ptr op) -> register_result {
  reactor_op_ptr old{};
  fd_interest interest{};
  std::uint64_t token = 0;

  {
    std::scoped_lock lk{mtx_};
    auto it = operations_.find(fd);
    if (it == operations_.end() && !op) {
      return register_result{};
    }
    if (it == operations_.end()) {
      it = operations_.emplace(fd, fd_ops{}).first;
    }

    auto& ops = it->second;
    if (op) {
      token = ops.read_token = next_token_++;
    }
    old = std::exchange(ops.read_op, std::move(op));
    interest = interest_for(ops);

    if (!interest.want_read && !interest.want_write) {
      operations_.erase(it);
    }
  }

  return register_result{token, std::move(old), interest};
}

inline auto fd_registry::register_write(int fd, reactor_op_ptr op) -> register_result {
  reactor_op_ptr old{};
  fd_interest interest{};
  std::uint64_t token = 0;

  {
    std::scoped_lock lk{mtx_};
    auto it = operations_.find(fd);
    if (it == operations_.end() && !op) {
      return register_result{};
    }
    if (it == operations_.end()) {
      it = operations_.emplace(fd, fd_ops{}).first;
    }

    auto& ops = it->second;
    if (op) {
      token = ops.write_token = next_token_++;
    }
    old = std::exchange(ops.write_op, std::move(op));
    interest = interest_for(ops);

    if (!interest.want_read && !interest.want_write) {
      operations_.erase(it);
    }
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
    auto it = operations_.find(fd);
    if (it == operations_.end()) {
      return cancel_result{};
    }

    auto& ops = it->second;
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
    if (!interest.want_read && !interest.want_write) {
      operations_.erase(it);
    }
  }

  return cancel_result{std::move(removed), interest, matched};
}

inline auto fd_registry::deregister(int fd) -> deregister_result {
  reactor_op_ptr read{};
  reactor_op_ptr write{};
  bool had_any = false;

  {
    std::scoped_lock lk{mtx_};
    auto it = operations_.find(fd);
    if (it != operations_.end()) {
      read = std::move(it->second.read_op);
      write = std::move(it->second.write_op);
      had_any = true;
      operations_.erase(it);
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
    auto it = operations_.find(fd);
    if (it == operations_.end()) {
      return ready_result{};
    }

    if (can_read) {
      read = std::move(it->second.read_op);
      it->second.read_token = 0;
    }
    if (can_write) {
      write = std::move(it->second.write_op);
      it->second.write_token = 0;
    }

    interest = interest_for(it->second);
    if (!interest.want_read && !interest.want_write) {
      operations_.erase(it);
    }
  }

  return ready_result{ready_ops{std::move(read), std::move(write)}, interest};
}

inline auto fd_registry::empty() const -> bool {
  std::scoped_lock lk{mtx_};
  return operations_.empty();
}

}  // namespace iocoro::detail
