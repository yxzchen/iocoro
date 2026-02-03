#pragma once

#include <stop_token>
#include <utility>

namespace iocoro {

/// A resettable stop/cancellation scope.
///
/// `std::stop_source` itself cannot be reset; this wrapper provides a convenient `reset()`
/// that swaps in a new source.
class stop_scope {
 public:
  stop_scope() = default;

  auto get_token() const noexcept -> std::stop_token { return src_.get_token(); }
  auto token() const noexcept -> std::stop_token { return get_token(); }

  void request_stop() noexcept { src_.request_stop(); }

  void reset() { src_ = std::stop_source{}; }

 private:
  std::stop_source src_{};
};

}  // namespace iocoro

