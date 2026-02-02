#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

namespace iocoro::detail {

struct backend_event {
  int fd = -1;
  bool can_read = false;
  bool can_write = false;
  bool is_error = false;
  std::error_code ec{};
};

class backend_interface {
 public:
  virtual ~backend_interface() = default;

  backend_interface() = default;
  backend_interface(backend_interface const&) = delete;
  auto operator=(backend_interface const&) -> backend_interface& = delete;
  backend_interface(backend_interface&&) = delete;
  auto operator=(backend_interface&&) -> backend_interface& = delete;

  virtual void update_fd_interest(int fd, bool want_read, bool want_write) = 0;
  virtual void remove_fd_interest(int fd) noexcept = 0;

  virtual auto wait(std::optional<std::chrono::milliseconds> timeout,
                    std::vector<backend_event>& out) -> void = 0;
  virtual void wakeup() noexcept = 0;
};

// Backend selection:
// - Default is epoll (no additional dependencies).
// - Define `IOCORO_BACKEND_URING` to use io_uring. This requires liburing headers and linking
//   against liburing.
// - Define `IOCORO_BACKEND_EPOLL` to force epoll explicitly.
auto make_backend() -> std::unique_ptr<backend_interface>;

}  // namespace iocoro::detail
