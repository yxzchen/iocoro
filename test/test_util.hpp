#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/work_guard.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace iocoro::test {

struct unique_fd {
  int fd{-1};

  unique_fd() noexcept = default;
  explicit unique_fd(int v) noexcept : fd(v) {}

  unique_fd(unique_fd const&) = delete;
  auto operator=(unique_fd const&) -> unique_fd& = delete;

  unique_fd(unique_fd&& other) noexcept : fd(other.fd) { other.fd = -1; }
  auto operator=(unique_fd&& other) noexcept -> unique_fd& {
    if (this != &other) {
      reset();
      fd = other.fd;
      other.fd = -1;
    }
    return *this;
  }

  ~unique_fd() { reset(); }

  auto get() const noexcept -> int { return fd; }

  void reset() noexcept {
    if (fd >= 0) {
      (void)::close(fd);
      fd = -1;
    }
  }
};

inline auto make_listen_socket_ipv4() -> std::pair<unique_fd, std::uint16_t> {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return {unique_fd{}, 0};
  }

  int opt = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    (void)::close(fd);
    return {unique_fd{}, 0};
  }

  if (::listen(fd, 16) != 0) {
    (void)::close(fd);
    return {unique_fd{}, 0};
  }

  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    (void)::close(fd);
    return {unique_fd{}, 0};
  }

  return {unique_fd{fd}, ntohs(addr.sin_port)};
}

inline auto make_temp_path(std::string_view prefix) -> std::string {
  static std::atomic<unsigned int> counter{0};
  auto const id = counter.fetch_add(1);
  return std::string{"/tmp/"} + std::string{prefix} + "_" + std::to_string(::getpid()) + "_" +
         std::to_string(id);
}

inline void unlink_path(std::string const& path) noexcept {
  (void)::unlink(path.c_str());
}

template <class T>
auto sync_wait(iocoro::io_context& ctx, iocoro::awaitable<T> a)
  -> iocoro::expected<T, std::exception_ptr> {
  std::optional<iocoro::expected<T, std::exception_ptr>> result;

  // Keep the io_context alive until the completion handler runs.
  // Without this, ctx.run() may return early (no pending work), and we would
  // hit UB by dereferencing `result` before it's set.
  iocoro::work_guard<iocoro::any_io_executor> wg{ctx.get_executor()};

  iocoro::co_spawn(ctx.get_executor(), std::move(a),
                   [&](iocoro::expected<T, std::exception_ptr> r) {
                     result = std::move(r);
                     wg.reset();
                   });

  ctx.run();
  ctx.restart();

  return std::move(*result);
}

}  // namespace iocoro::test
