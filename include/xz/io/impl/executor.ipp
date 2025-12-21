#include <xz/io/assert.hpp>
#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/executor.hpp>

#include <stdexcept>

namespace xz::io {

executor::executor(detail::io_context_impl& impl) noexcept : impl_{&impl} {}

executor::executor() noexcept : impl_{nullptr} {}

void executor::execute(std::function<void()> f) const { ensure_impl().post(std::move(f)); }
void executor::post(std::function<void()> f) const { ensure_impl().post(std::move(f)); }
void executor::dispatch(std::function<void()> f) const { ensure_impl().dispatch(std::move(f)); }

void executor::add_work_guard() const noexcept {
  // Work guards are best-effort; if an executor is empty, it simply can't guard anything.
  if (impl_ != nullptr) impl_->add_work_guard();
}
void executor::remove_work_guard() const noexcept {
  if (impl_ != nullptr) impl_->remove_work_guard();
}

auto executor::ensure_impl() const -> detail::io_context_impl& {
  IOXZ_ENSURE(impl_, "[ioxz] executor: empty executor");
  return *impl_;
}

}  // namespace xz::io
