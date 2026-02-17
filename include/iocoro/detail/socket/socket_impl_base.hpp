#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/operation_awaiter.hpp>
#include <iocoro/detail/socket/fd_resource.hpp>
#include <iocoro/error.hpp>
#include <iocoro/result.hpp>
#include <iocoro/socket_option.hpp>
#include <iocoro/this_coro.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>

#include <sys/socket.h>
#include <cerrno>

namespace iocoro::detail::socket {

/// Base class for socket-like implementations.
///
/// Responsibilities:
/// - Own IO executor binding.
/// - Manage native-fd lifecycle via shared `fd_resource`.
/// - Provide thread-safe cancel/close/release primitives.
///
/// Semantics:
/// - `close()` is logical-close: blocks new operations and cancels pending waits.
/// - Physical `::close(fd)` happens when the last holder of the old `fd_resource` is released.
class socket_impl_base {
 public:
  using event_handle = io_context_impl::event_handle;

  class operation_guard {
   public:
    operation_guard() noexcept = default;

    operation_guard(operation_guard const&) = delete;
    auto operator=(operation_guard const&) -> operation_guard& = delete;

    operation_guard(operation_guard&& other) noexcept : res_(std::move(other.res_)) {}
    auto operator=(operation_guard&& other) noexcept -> operation_guard& {
      if (this != &other) {
        reset();
        res_ = std::move(other.res_);
      }
      return *this;
    }

    ~operation_guard() noexcept { reset(); }

    explicit operator bool() const noexcept { return static_cast<bool>(res_); }

    auto resource() const noexcept -> std::shared_ptr<fd_resource> const& { return res_; }

    void reset() noexcept {
      if (res_) {
        res_->remove_inflight();
        res_.reset();
      }
    }

   private:
    explicit operation_guard(std::shared_ptr<fd_resource> res) noexcept : res_(std::move(res)) {}

    friend class socket_impl_base;
    std::shared_ptr<fd_resource> res_{};
  };

  socket_impl_base() noexcept = delete;
  explicit socket_impl_base(any_io_executor ex) noexcept
      : ex_(std::move(ex)), dispatch_ex_(any_executor{ex_}), ctx_impl_(ex_.io_context_ptr()) {
    IOCORO_ENSURE(ex_, "socket_impl_base: requires IO executor");
    IOCORO_ENSURE(ctx_impl_, "socket_impl_base: requires IO executor");
  }

  socket_impl_base(socket_impl_base const&) = delete;
  auto operator=(socket_impl_base const&) -> socket_impl_base& = delete;
  socket_impl_base(socket_impl_base&&) = delete;
  auto operator=(socket_impl_base&&) -> socket_impl_base& = delete;

  ~socket_impl_base() noexcept { (void)close(); }

  auto get_io_context_impl() const noexcept -> io_context_impl* { return ctx_impl_; }
  auto get_executor() const noexcept -> any_io_executor { return ex_; }

  /// Native handle snapshot. Returns -1 if closed.
  auto native_handle() const noexcept -> int {
    auto res = acquire_resource();
    if (!res) {
      return -1;
    }
    return res->native_handle();
  }

  auto is_open() const noexcept -> bool { return native_handle() >= 0; }

  auto acquire_resource() const noexcept -> std::shared_ptr<fd_resource> {
    return res_.load(std::memory_order_acquire);
  }

  auto make_operation_guard(std::shared_ptr<fd_resource> const& res) noexcept -> operation_guard {
    if (!res) {
      return {};
    }

    std::scoped_lock lk{lifecycle_mtx_};
    auto current = res_.load(std::memory_order_acquire);
    if (!current || current.get() != res.get() || current->closing() ||
        current->native_handle() < 0) {
      return {};
    }

    current->add_inflight();

    return operation_guard{std::move(current)};
  }

  auto open(int domain, int type, int protocol) noexcept -> result<void>;
  auto assign(int fd) noexcept -> result<void>;

  template <class Option>
  auto set_option(Option const& opt) -> result<void> {
    auto const fd = native_handle();
    if (fd < 0) {
      return fail(error::not_open);
    }

    if (::setsockopt(fd, opt.level(), opt.name(), opt.data(), opt.size()) != 0) {
      return fail(std::error_code(errno, std::generic_category()));
    }
    return ok();
  }

  template <class Option>
  auto get_option(Option& opt) -> result<void> {
    auto const fd = native_handle();
    if (fd < 0) {
      return fail(error::not_open);
    }

    socklen_t len = opt.size();
    if (::getsockopt(fd, opt.level(), opt.name(), opt.data(), &len) != 0) {
      return fail(std::error_code(errno, std::generic_category()));
    }
    return ok();
  }

  void cancel() noexcept;
  void cancel_read() noexcept;
  void cancel_write() noexcept;

  auto close() noexcept -> result<void>;

  auto has_pending_operations() const noexcept -> bool {
    auto res = acquire_resource();
    return res && res->inflight_count() > 0;
  }

  /// Release ownership of the native handle without closing it.
  ///
  /// Returns:
  /// - fd on success
  /// - error::busy if there are in-flight operations
  /// - error::not_open if already closed
  auto release() noexcept -> result<int>;

  auto wait_read_ready() -> awaitable<result<void>> {
    auto res = acquire_resource();
    return wait_ready_impl(std::move(res), true);
  }

  auto wait_write_ready() -> awaitable<result<void>> {
    auto res = acquire_resource();
    return wait_ready_impl(std::move(res), false);
  }

  auto wait_read_ready(std::shared_ptr<fd_resource> const& res) -> awaitable<result<void>> {
    return wait_ready_impl(res, true);
  }

  auto wait_write_ready(std::shared_ptr<fd_resource> const& res) -> awaitable<result<void>> {
    return wait_ready_impl(res, false);
  }

 private:
  auto wait_ready_impl(std::shared_ptr<fd_resource> const& res,
                       bool is_read) -> awaitable<result<void>> {
    auto inflight = make_operation_guard(res);
    if (!inflight) {
      if (res && res->closing()) {
        co_return unexpected(error::operation_aborted);
      }
      co_return unexpected(error::not_open);
    }
    auto pinned = inflight.resource();

    co_await this_coro::on(dispatch_ex_);
    auto const cancel_epoch = is_read ? pinned->read_cancel_epoch() : pinned->write_cancel_epoch();
    auto r = co_await detail::operation_awaiter{
      [this, pinned, is_read, cancel_epoch](detail::reactor_op_ptr rop) mutable {
        event_handle h = is_read
                           ? ctx_impl_->register_fd_read(pinned->native_handle(), std::move(rop))
                           : ctx_impl_->register_fd_write(pinned->native_handle(), std::move(rop));
        if (is_read) {
          pinned->set_read_handle(h, cancel_epoch);
        } else {
          pinned->set_write_handle(h, cancel_epoch);
        }
        return h;
      }};
    if (r && pinned->closing()) {
      co_return unexpected(error::operation_aborted);
    }
    co_return r;
  }
  static void mark_closing(std::shared_ptr<fd_resource> const& res) noexcept {
    if (!res) {
      return;
    }
    res->mark_closing();
    res->cancel_all_handles();
  }

  any_io_executor ex_{};
  any_executor dispatch_ex_{};
  io_context_impl* ctx_impl_{};

  mutable std::mutex lifecycle_mtx_{};
  std::atomic<std::shared_ptr<fd_resource>> res_{};
};

}  // namespace iocoro::detail::socket

#include <iocoro/impl/socket/socket_impl_base.ipp>
