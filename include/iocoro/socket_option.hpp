#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Native socket option constants/types.
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace iocoro::socket_option {

/// Fixed-size socket option wrapper.
///
/// Models the minimal interface required by `setsockopt/getsockopt`:
/// - level()
/// - name()
/// - data()/size() for setting
/// - data()/size() for getting (mutable)
template <int Level, int Name, class T>
class option {
 public:
  using value_type = T;

  constexpr option() noexcept(std::is_nothrow_default_constructible_v<T>) = default;
  constexpr explicit option(T v) noexcept(std::is_nothrow_move_constructible_v<T>)
      : value_(std::move(v)) {}

  static constexpr auto level() noexcept -> int { return Level; }
  static constexpr auto name() noexcept -> int { return Name; }

  constexpr auto value() const noexcept -> T const& { return value_; }
  constexpr auto value() noexcept -> T& { return value_; }
  constexpr void value(T v) noexcept(std::is_nothrow_move_assignable_v<T>) { value_ = std::move(v); }

  auto data() noexcept -> void* { return static_cast<void*>(std::addressof(value_)); }
  auto data() const noexcept -> void const* { return static_cast<void const*>(std::addressof(value_)); }
  static constexpr auto size() noexcept -> socklen_t { return static_cast<socklen_t>(sizeof(T)); }

 private:
  T value_{};
};

/// Boolean socket option (stored as int for POSIX compatibility).
template <int Level, int Name>
class boolean_option : public option<Level, Name, int> {
 public:
  using base = option<Level, Name, int>;
  using base::base;

  constexpr boolean_option() noexcept = default;
  constexpr explicit boolean_option(bool enabled) noexcept : base(enabled ? 1 : 0) {}

  constexpr auto enabled() const noexcept -> bool { return this->value() != 0; }
  constexpr void enabled(bool on) noexcept { this->value(on ? 1 : 0); }
};

// Common socket options (protocol-agnostic).
using reuse_address = boolean_option<SOL_SOCKET, SO_REUSEADDR>;
using keep_alive = boolean_option<SOL_SOCKET, SO_KEEPALIVE>;

using send_buffer_size = option<SOL_SOCKET, SO_SNDBUF, int>;
using receive_buffer_size = option<SOL_SOCKET, SO_RCVBUF, int>;

// Linger uses struct linger (POSIX).
using linger = option<SOL_SOCKET, SO_LINGER, ::linger>;
namespace tcp {
// TCP specific.
using no_delay = boolean_option<IPPROTO_TCP, TCP_NODELAY>;
}  // namespace tcp

}  // namespace iocoro::socket_option
