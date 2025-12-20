#include <xz/io/detail/context/io_context_impl.hpp>
#include <xz/io/executor.hpp>

#include <stdexcept>

namespace xz::io {

executor::executor(detail::io_context_impl& impl) noexcept : impl_{&impl} {}

void executor::execute(std::function<void()> f) const { impl_->post(std::move(f)); }
void executor::post(std::function<void()> f) const { impl_->post(std::move(f)); }
void executor::dispatch(std::function<void()> f) const { impl_->dispatch(std::move(f)); }

void executor::add_work_guard() const noexcept { impl_->add_work_guard(); }
void executor::remove_work_guard() const noexcept { impl_->remove_work_guard(); }

}  // namespace xz::io
