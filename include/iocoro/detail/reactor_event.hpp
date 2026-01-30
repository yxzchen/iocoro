#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <variant>

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

struct event_desc {
  enum class kind : std::uint8_t { timer, fd_read, fd_write };
  kind type{};
  std::chrono::steady_clock::time_point expiry{};
  int fd{-1};

  static auto timer(std::chrono::steady_clock::time_point tp) noexcept -> event_desc {
    return event_desc{kind::timer, tp, -1};
  }
  static auto fd_read(int handle) noexcept -> event_desc {
    return event_desc{kind::fd_read, {}, handle};
  }
  static auto fd_write(int handle) noexcept -> event_desc {
    return event_desc{kind::fd_write, {}, handle};
  }
};

struct event_handle {
  std::variant<std::monostate, timer_event_handle, fd_event_handle> handle{};

  auto valid() const noexcept -> bool {
    return std::visit(
      [](auto const& h) -> bool {
        if constexpr (std::is_same_v<std::decay_t<decltype(h)>, std::monostate>) {
          return false;
        } else {
          return h.valid();
        }
      },
      handle);
  }
  explicit operator bool() const noexcept { return valid(); }

  void cancel() const noexcept {
    std::visit(
      [](auto const& h) noexcept {
        if constexpr (!std::is_same_v<std::decay_t<decltype(h)>, std::monostate>) {
          if (h) {
            h.cancel();
          }
        }
      },
      handle);
  }

  auto as_timer() const noexcept -> timer_event_handle {
    if (auto p = std::get_if<timer_event_handle>(&handle)) {
      return *p;
    }
    return timer_event_handle::invalid_handle();
  }

  auto as_fd() const noexcept -> fd_event_handle {
    if (auto p = std::get_if<fd_event_handle>(&handle)) {
      return *p;
    }
    return fd_event_handle::invalid_handle();
  }
};

}  // namespace iocoro::detail
