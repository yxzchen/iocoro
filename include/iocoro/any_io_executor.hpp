#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/assert.hpp>
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

  explicit any_io_executor(any_executor ex) noexcept : storage_(ex.storage_) {
    if (storage_) {
      IOCORO_ENSURE(has_capability(storage_.capabilities(), executor_capability::io),
                   "any_io_executor: requires IO-capable executor");
      impl_ = storage_.io_context_ptr();
      IOCORO_ENSURE(impl_ != nullptr, "any_io_executor: missing io_context_impl");
    }
  }

  template <executor Ex>
  explicit any_io_executor(Ex ex) noexcept : any_io_executor(any_executor{std::move(ex)}) {}

  /// Schedule work for later execution.
  void post(detail::unique_function<void()> f) const noexcept { storage_.post(std::move(f)); }

  /// Execute inline when permitted; otherwise schedule like `post()`.
  void dispatch(detail::unique_function<void()> f) const noexcept { storage_.dispatch(std::move(f)); }

  auto capabilities() const noexcept -> executor_capability { return storage_.capabilities(); }

  /// True if the underlying `io_context` has been stopped (or this executor is empty).
  auto stopped() const noexcept -> bool { return impl_ == nullptr || impl_->stopped(); }

  explicit operator bool() const noexcept { return static_cast<bool>(storage_); }

  auto as_any_executor() const noexcept -> any_executor { return any_executor{*this}; }

  /// Access the underlying `io_context` implementation (for internal integrations).
  auto io_context_ptr() const noexcept -> detail::io_context_impl* { return impl_; }

  friend auto operator==(any_io_executor const& a, any_io_executor const& b) noexcept -> bool {
    return a.storage_ == b.storage_;
  }

  friend auto operator!=(any_io_executor const& a, any_io_executor const& b) noexcept -> bool {
    return !(a == b);
  }

 private:
  template <typename>
  friend class work_guard;
  friend class io_context;
  friend class any_executor;

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

  detail::any_executor_storage storage_{};
  detail::io_context_impl* impl_ = nullptr;
};

inline any_executor::any_executor(any_io_executor const& ex) noexcept
    : any_executor(storage_tag{}, ex.storage_) {}

}  // namespace iocoro

namespace iocoro::detail {

template <>
struct executor_traits<any_io_executor> {
  static auto capabilities(any_io_executor const& ex) noexcept -> executor_capability {
    if (!ex) {
      return executor_capability::none;
    }
    return ex.capabilities();
  }

  static auto io_context(any_io_executor const& ex) noexcept -> io_context_impl* {
    return ex.io_context_ptr();
  }
};

}  // namespace iocoro::detail
