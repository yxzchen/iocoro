#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/io_executor.hpp>

#include <stdexcept>

namespace iocoro {

inline io_executor::io_executor(detail::io_context_impl& impl) noexcept : impl_{&impl} {}

inline io_executor::io_executor() noexcept : impl_{nullptr} {}

inline void io_executor::execute(std::function<void()> f) const { ensure_impl().post(std::move(f)); }
inline void io_executor::post(std::function<void()> f) const { ensure_impl().post(std::move(f)); }
inline void io_executor::dispatch(std::function<void()> f) const {
  ensure_impl().dispatch(std::move(f));
}

inline auto io_executor::stopped() const noexcept -> bool {
  return impl_ == nullptr || impl_->stopped();
}

inline void io_executor::add_work_guard() const noexcept {
  // Work guards are best-effort; if an io_executor is empty, it simply can't guard anything.
  if (impl_ != nullptr) {
    impl_->add_work_guard();
  }
}
inline void io_executor::remove_work_guard() const noexcept {
  if (impl_ != nullptr) {
    impl_->remove_work_guard();
  }
}

inline auto io_executor::ensure_impl() const -> detail::io_context_impl& {
  IOCORO_ENSURE(impl_, "io_executor: empty impl_");
  return *impl_;
}

}  // namespace iocoro
