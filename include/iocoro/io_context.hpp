#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/any_io_executor.hpp>
#include <iocoro/detail/executor_guard.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/unique_function.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace iocoro {

template <typename Executor>
class work_guard;

/// The execution context for asynchronous I/O operations
class io_context {
 public:
  /// Internal executor type bound to this io_context.
  class executor_type {
   public:
    executor_type() noexcept : impl_{nullptr} {}
    explicit executor_type(detail::io_context_impl& impl) noexcept : impl_{&impl} {}

    executor_type(executor_type const&) noexcept = default;
    auto operator=(executor_type const&) noexcept -> executor_type& = default;
    executor_type(executor_type&&) noexcept = default;
    auto operator=(executor_type&&) noexcept -> executor_type& = default;

    template <class F>
      requires std::is_invocable_v<F&>
    void post(F&& f) const noexcept {
      ensure_impl().post([ex = *this, f = std::move(f)]() mutable {
        detail::executor_guard g{any_executor{ex}};
        f();
      });
    }

    template <class F>
      requires std::is_invocable_v<F&>
    void dispatch(F&& f) const noexcept {
      ensure_impl().dispatch([ex = *this, f = std::move(f)]() mutable {
        detail::executor_guard g{any_executor{ex}};
        f();
      });
    }

    auto stopped() const noexcept -> bool { return impl_ == nullptr || impl_->stopped(); }

    explicit operator bool() const noexcept { return impl_ != nullptr; }

    friend auto operator==(executor_type const& a, executor_type const& b) noexcept -> bool {
      return a.impl_ == b.impl_;
    }

    friend auto operator!=(executor_type const& a, executor_type const& b) noexcept -> bool {
      return !(a == b);
    }

   private:
    template <typename>
    friend class work_guard;
    friend class io_context;
    friend struct detail::executor_traits<executor_type>;

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

    auto ensure_impl() const -> detail::io_context_impl& {
      IOCORO_ENSURE(impl_, "io_context::executor_type: empty impl_");
      return *impl_;
    }

    // Non-owning pointer. The associated io_context_impl must outlive this executor.
    detail::io_context_impl* impl_;
  };

  io_context() : impl_(std::make_unique<detail::io_context_impl>()) {}
  ~io_context() = default;

  io_context(io_context const&) = delete;
  auto operator=(io_context const&) -> io_context& = delete;
  io_context(io_context&&) = delete;
  auto operator=(io_context&&) -> io_context& = delete;

  auto run() -> std::size_t { return impl_->run(); }
  auto run_one() -> std::size_t { return impl_->run_one(); }
  auto run_for(std::chrono::milliseconds timeout) -> std::size_t { return impl_->run_for(timeout); }

  void stop() { impl_->stop(); }
  void restart() { impl_->restart(); }
  auto stopped() const noexcept -> bool { return impl_->stopped(); }

  /// Get an IO-capable executor associated with this io_context
  auto get_executor() noexcept -> any_io_executor {
    return any_io_executor{executor_type{*impl_}};
  }

 private:
  std::unique_ptr<detail::io_context_impl> impl_;
};

}  // namespace iocoro

namespace iocoro::detail {

template <>
struct executor_traits<iocoro::io_context::executor_type> {
  static auto capabilities(iocoro::io_context::executor_type const& ex) noexcept
    -> executor_capability {
    return ex ? executor_capability::io : executor_capability::none;
  }

  static auto io_context(iocoro::io_context::executor_type const& ex) noexcept
    -> io_context_impl* {
    return ex.impl_;
  }
};

}  // namespace iocoro::detail
