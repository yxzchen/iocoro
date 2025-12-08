#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/steady_timer.hpp>

namespace xz::io::detail {

class steady_timer_impl {
 public:
  explicit steady_timer_impl(io_context& ctx) : ctx_(ctx) {}

  auto get_executor() noexcept -> io_context& { return ctx_; }

  void cancel() {
    if (timer_id_ != 0) {
      ctx_.cancel_timer(timer_id_);
      timer_id_ = 0;
    }
  }

  auto wait(steady_timer::duration d) -> std::error_code;

  uint64_t timer_id_ = 0;

 private:
  io_context& ctx_;
};

}  // namespace xz::io::detail
