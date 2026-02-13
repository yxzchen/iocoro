#pragma once

#include <iocoro/any_io_executor.hpp>
#include <iocoro/detail/reactor_types.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

#include <unistd.h>
#include <cerrno>

namespace iocoro::detail::socket {

/// Shared native-fd ownership for socket operations.
///
/// Key semantics:
/// - The fd stays alive while any in-flight operation holds a shared_ptr to this object.
/// - `mark_closing()` transitions the resource into logical-close mode; new waits are cancelled.
/// - Physical `::close(fd)` happens in the destructor (or after `release_fd()` is called).
class fd_resource {
 public:
  fd_resource(any_io_executor ex, int fd) noexcept : ex_(std::move(ex)), fd_(fd) {}

  fd_resource(fd_resource const&) = delete;
  auto operator=(fd_resource const&) -> fd_resource& = delete;
  fd_resource(fd_resource&&) = delete;
  auto operator=(fd_resource&&) -> fd_resource& = delete;

  ~fd_resource() noexcept {
    cancel_all_handles();

    auto fd = release_fd();
    if (fd < 0) {
      return;
    }

    auto* ctx = ex_.io_context_ptr();
    if (ctx != nullptr) {
      ctx->remove_fd(fd);
    }

    ::close(fd);
  }

  void mark_closing() noexcept { closing_.store(true, std::memory_order_release); }
  auto closing() const noexcept -> bool { return closing_.load(std::memory_order_acquire); }

  auto native_handle() const noexcept -> int { return fd_.load(std::memory_order_acquire); }

  auto release_fd() noexcept -> int { return fd_.exchange(-1, std::memory_order_acq_rel); }

  void add_inflight() noexcept { inflight_.fetch_add(1, std::memory_order_acq_rel); }

  void remove_inflight() noexcept { inflight_.fetch_sub(1, std::memory_order_acq_rel); }

  auto inflight_count() const noexcept -> std::uint32_t {
    return inflight_.load(std::memory_order_acquire);
  }

  void set_read_handle(event_handle h) noexcept {
    bool accept = false;
    {
      std::scoped_lock lk{mtx_};
      if (!closing_.load(std::memory_order_acquire) && native_handle() >= 0) {
        read_handle_ = h;
        accept = true;
      }
    }
    if (!accept && h) {
      h.cancel();
    }
  }

  void set_write_handle(event_handle h) noexcept {
    bool accept = false;
    {
      std::scoped_lock lk{mtx_};
      if (!closing_.load(std::memory_order_acquire) && native_handle() >= 0) {
        write_handle_ = h;
        accept = true;
      }
    }
    if (!accept && h) {
      h.cancel();
    }
  }

  void cancel_read_handle() noexcept {
    event_handle h{};
    {
      std::scoped_lock lk{mtx_};
      h = std::exchange(read_handle_, {});
    }
    h.cancel();
  }

  void cancel_write_handle() noexcept {
    event_handle h{};
    {
      std::scoped_lock lk{mtx_};
      h = std::exchange(write_handle_, {});
    }
    h.cancel();
  }

  void cancel_all_handles() noexcept {
    event_handle rh{};
    event_handle wh{};
    {
      std::scoped_lock lk{mtx_};
      rh = std::exchange(read_handle_, {});
      wh = std::exchange(write_handle_, {});
    }
    rh.cancel();
    wh.cancel();
  }

 private:
  any_io_executor ex_{};
  std::atomic<int> fd_{-1};
  std::atomic<bool> closing_{false};
  std::atomic<std::uint32_t> inflight_{0};
  mutable std::mutex mtx_{};
  event_handle read_handle_{};
  event_handle write_handle_{};
};

}  // namespace iocoro::detail::socket
