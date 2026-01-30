#pragma once

#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/unique_function.hpp>
#include <iocoro/any_executor.hpp>

#include <exception>
#include <type_traits>
#include <utility>

namespace iocoro {

class steady_timer;

namespace detail::socket {
class socket_impl_base;
}  // namespace detail::socket

namespace detail {
struct io_executor_access;
}  // namespace detail

template <typename Executor>
class work_guard;

/// Executor interface for executing work on an io_context
class io_executor {
 public:
  /// Default-constructed io_executor is an "empty" io_executor and must be assigned
  /// a valid context before use.
  io_executor() noexcept : impl_{nullptr} {}
  explicit io_executor(detail::io_context_impl& impl) noexcept : impl_{&impl} {}

  io_executor(io_executor const&) noexcept = default;
  auto operator=(io_executor const&) noexcept -> io_executor& = default;
  io_executor(io_executor&&) noexcept = default;
  auto operator=(io_executor&&) noexcept -> io_executor& = default;

  /// Post the function for later execution (never inline).
  template <class F>
    requires std::is_invocable_v<F&>
  void post(F&& f) const noexcept {
    ensure_impl().post([ex = *this, f = std::move(f)]() mutable {
      detail::executor_guard g{any_executor{ex}};
      f();
    });
  }

  /// Dispatch the function (inline if in context thread, otherwise queued).
  template <class F>
    requires std::is_invocable_v<F&>
  void dispatch(F&& f) const noexcept {
    ensure_impl().dispatch([ex = *this, f = std::move(f)]() mutable {
      detail::executor_guard g{ex};
      f();
    });
  }

  /// Returns true if the associated context is stopped (or io_executor is empty).
  auto stopped() const noexcept -> bool { return impl_ == nullptr || impl_->stopped(); }

  explicit operator bool() const noexcept { return impl_ != nullptr; }

  friend auto operator==(io_executor const& a, io_executor const& b) noexcept -> bool {
    return a.impl_ == b.impl_;
  }

  friend auto operator!=(io_executor const& a, io_executor const& b) noexcept -> bool {
    return a.impl_ != b.impl_;
  }

 private:
  template <typename>
  friend class work_guard;

  friend class steady_timer;
  friend class detail::socket::socket_impl_base;
  friend struct detail::io_executor_access;
  friend struct detail::executor_traits<io_executor>;

  void add_work_guard() const noexcept {
    // Work guards are best-effort; if an io_executor is empty, it simply can't guard anything.
    if (impl_ != nullptr) {
      impl_->add_work_guard();
    }
  }
  void remove_work_guard() const noexcept {
    if (impl_ != nullptr) {
      impl_->remove_work_guard();
    }
  }

  auto ensure_impl() const -> detail::io_context_impl& {
    IOCORO_ENSURE(impl_, "io_executor: empty impl_");
    return *impl_;
  }

  // Non-owning pointer. The associated io_context_impl must outlive the io_executor.
  detail::io_context_impl* impl_;
};

}  // namespace iocoro

namespace iocoro::detail {

template <>
struct executor_traits<io_executor> {
  static auto capabilities(io_executor const& ex) noexcept -> executor_capability {
    return ex ? executor_capability::io : executor_capability::none;
  }

  static auto io_context(io_executor const& ex) noexcept -> io_context_impl* { return ex.impl_; }
};

}  // namespace iocoro::detail
