#include <iocoro/assert.hpp>
#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/executor.hpp>

#include <stdexcept>

namespace iocoro {

inline executor::executor(detail::io_context_impl& impl) noexcept : impl_{&impl} {}

inline executor::executor() noexcept : impl_{nullptr} {}

inline void executor::execute(std::function<void()> f) const { ensure_impl().post(std::move(f)); }
inline void executor::post(std::function<void()> f) const { ensure_impl().post(std::move(f)); }
inline void executor::dispatch(std::function<void()> f) const {
  ensure_impl().dispatch(std::move(f));
}

inline auto executor::stopped() const noexcept -> bool {
  return impl_ == nullptr || impl_->stopped();
}

inline void executor::add_work_guard() const noexcept {
  // Work guards are best-effort; if an executor is empty, it simply can't guard anything.
  if (impl_ != nullptr) {
    impl_->add_work_guard();
  }
}
inline void executor::remove_work_guard() const noexcept {
  if (impl_ != nullptr) {
    impl_->remove_work_guard();
  }
}

inline auto executor::ensure_impl() const -> detail::io_context_impl& {
  IOCORO_ENSURE(impl_, "executor: empty impl_");
  return *impl_;
}

}  // namespace iocoro
