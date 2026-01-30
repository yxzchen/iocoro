#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
#include <iocoro/detail/executor_cast.hpp>
#include <iocoro/detail/io_context_impl.hpp>

#include <type_traits>
#include <utility>

namespace iocoro {

class io_context;

/// Type-erased IO-capable executor.
///
/// Semantics:
/// - Must wrap an executor that supports IO (supports_io() == true).
/// - Empty executor is allowed; operations become no-ops.
class any_io_executor {
 public:
  any_io_executor() noexcept = default;

  explicit any_io_executor(any_executor ex) noexcept : ex_(std::move(ex)) {
    if (ex_) {
      IOCORO_ENSURE(ex_.supports_io(), "any_io_executor: requires IO-capable executor");
      impl_ = detail::any_executor_access::io_context(ex_);
      IOCORO_ENSURE(impl_ != nullptr, "any_io_executor: missing io_context_impl");
    }
  }

  template <executor Ex>
  explicit any_io_executor(Ex ex) noexcept : any_io_executor(any_executor{std::move(ex)}) {}

  template <class F>
    requires std::is_invocable_v<F&>
  void post(F&& f) const noexcept {
    if (!ex_) {
      return;
    }
    ex_.post(std::forward<F>(f));
  }

  template <class F>
    requires std::is_invocable_v<F&>
  void dispatch(F&& f) const noexcept {
    if (!ex_) {
      return;
    }
    ex_.dispatch(std::forward<F>(f));
  }

  auto stopped() const noexcept -> bool { return impl_ == nullptr || impl_->stopped(); }

  explicit operator bool() const noexcept { return static_cast<bool>(ex_); }

  auto as_any_executor() const noexcept -> any_executor const& { return ex_; }

  friend auto operator==(any_io_executor const& a, any_io_executor const& b) noexcept -> bool {
    return a.ex_ == b.ex_;
  }

  friend auto operator!=(any_io_executor const& a, any_io_executor const& b) noexcept -> bool {
    return !(a == b);
  }

 private:
  template <typename>
  friend class work_guard;
  friend struct detail::executor_traits<any_io_executor>;
  friend class io_context;

  void add_work_guard() const noexcept {
    if (impl_ != nullptr) {
      impl_->add_work_guard();
    }
  }

  void remove_work_guard() const noexcept {
    if (impl_ != nullptr) {
      impl_->remove_work_guard();
    }
  }

  any_executor ex_{};
  detail::io_context_impl* impl_ = nullptr;
};

}  // namespace iocoro

namespace iocoro::detail {

template <>
struct executor_traits<any_io_executor> {
  static auto capabilities(any_io_executor const& ex) noexcept -> executor_capability {
    return ex ? executor_capability::io : executor_capability::none;
  }

  static auto io_context(any_io_executor const& ex) noexcept -> io_context_impl* {
    return ex.impl_;
  }
};

}  // namespace iocoro::detail
