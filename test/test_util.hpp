#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/work_guard.hpp>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace iocoro {

/// Exception thrown by sync_wait_for when the timeout expires.
class sync_wait_timeout : public std::runtime_error {
 public:
  explicit sync_wait_timeout(char const* msg) : std::runtime_error(msg) {}
};

namespace detail {

template <typename T>
struct sync_wait_state {
  bool done{false};
  std::exception_ptr ep{};
  std::optional<T> value{};
};

template <>
struct sync_wait_state<void> {
  bool done{false};
  std::exception_ptr ep{};
};

template <typename T>
inline void set_from_expected(sync_wait_state<T>& st, expected<T, std::exception_ptr>&& r) {
  st.done = true;
  if (!r) {
    st.ep = std::move(r).error();
    return;
  }
  st.value.emplace(std::move(*r));
}

inline void set_from_expected(sync_wait_state<void>& st,
                              expected<void, std::exception_ptr>&& r) noexcept {
  st.done = true;
  if (!r) st.ep = std::move(r).error();
}

template <typename T>
inline auto take_or_throw(sync_wait_state<T>& st) -> T {
  if (st.ep) std::rethrow_exception(st.ep);
  if constexpr (std::is_void_v<T>) {
    return;
  } else {
    if (!st.value.has_value()) {
      throw std::logic_error("sync_wait: missing value");
    }
    return std::move(*st.value);
  }
}

}  // namespace detail

/// Drive `ctx` until the awaitable completes; return its value or rethrow its exception.
///
/// Test utility (not part of the library API).
template <typename T>
auto sync_wait(io_context& ctx, awaitable<T> a) -> T {
  auto ex = ctx.get_executor();
  ctx.restart();

  // Create work_guard to prevent io_context from stopping before async operations complete.
  auto guard = make_work_guard(ex);

  detail::sync_wait_state<T> st{};
  co_spawn(ex, std::move(a), [&, guard = std::move(guard)](expected<T, std::exception_ptr> r) mutable {
    detail::set_from_expected(st, std::move(r));
    guard.reset();  // Release work_guard on completion
  });

  ctx.run();

  if (!st.done) throw std::logic_error("sync_wait: io_context stopped before completion");
  return detail::take_or_throw<T>(st);
}

/// Drive `ctx` for at most `timeout` until the awaitable completes; return its value or
/// rethrow its exception. Throws sync_wait_timeout on timeout.
///
/// Test utility (not part of the library API).
template <typename Rep, typename Period, typename T>
auto sync_wait_for(io_context& ctx, std::chrono::duration<Rep, Period> timeout, awaitable<T> a)
  -> T {
  auto ex = ctx.get_executor();
  ctx.restart();

  // Create work_guard to prevent io_context from stopping before async operations complete.
  auto guard = make_work_guard(ex);

  detail::sync_wait_state<T> st{};
  co_spawn(ex, std::move(a), [&, guard = std::move(guard)](expected<T, std::exception_ptr> r) mutable {
    detail::set_from_expected(st, std::move(r));
    guard.reset();  // Release work_guard on completion
  });

  ctx.run_for(std::chrono::duration_cast<std::chrono::milliseconds>(timeout));

  if (!st.done) {
    throw sync_wait_timeout("sync_wait_for: timeout");
  }

  ctx.run();

  return detail::take_or_throw<T>(st);
}

namespace test {

struct unique_fd {
  int fd{-1};
  unique_fd() = default;
  explicit unique_fd(int f) : fd(f) {}
  unique_fd(unique_fd const&) = delete;
  auto operator=(unique_fd const&) -> unique_fd& = delete;
  unique_fd(unique_fd&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
  auto operator=(unique_fd&& o) noexcept -> unique_fd& {
    if (this != &o) {
      reset();
      fd = std::exchange(o.fd, -1);
    }
    return *this;
  }
  ~unique_fd() { reset(); }
  void reset(int f = -1) noexcept {
    if (fd >= 0) {
      (void)::close(fd);
    }
    fd = f;
  }
  explicit operator bool() const noexcept { return fd >= 0; }
};

inline auto make_listen_socket_ipv4(std::uint16_t& port_out) -> unique_fd {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return unique_fd{};
  }

  // Best-effort: make accept loop non-blocking and avoid test hangs on failures.
  if (int flags = ::fcntl(fd, F_GETFL, 0); flags >= 0) {
    (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(0);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    (void)::close(fd);
    return unique_fd{};
  }

  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    (void)::close(fd);
    return unique_fd{};
  }

  if (::listen(fd, 16) != 0) {
    (void)::close(fd);
    return unique_fd{};
  }

  port_out = ntohs(addr.sin_port);
  return unique_fd{fd};
}

}  // namespace test

}  // namespace iocoro
