#pragma once

#include <iocoro/detail/operation_awaiter.hpp>
#include <iocoro/detail/operation_base.hpp>

#include <memory>
#include <system_error>

namespace iocoro::detail {

/// Generic async operation template that handles the common completion logic.
///
/// All async operations share the same pattern:
/// 1. on_ready() / on_abort() → complete(ec)
/// 2. complete() checks try_complete(), sets ec, posts to executor
/// 3. do_start() is operation-specific (registration logic)
class async_operation : public operation_base {
 public:
  void on_ready() noexcept final { complete(std::error_code{}); }
  void on_abort(std::error_code ec) noexcept final { complete(ec); }

 protected:
  async_operation(std::shared_ptr<operation_wait_state> st) noexcept
      : st_(std::move(st)) {}

  void complete(std::error_code ec) noexcept {
    // Guard against double completion (on_ready + on_abort, or repeated signals).
    if (done.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    st_->ec = ec;

    // Resume on the caller's original executor.
    // This ensures the awaitable coroutine resumes on the same executor
    // where the user initiated the wait operation.
    st_->ex.post([st = st_]() mutable { st->h.resume(); });
  }

  std::shared_ptr<operation_wait_state> st_;
  std::atomic<bool> done{false};
};

}  // namespace iocoro::detail
