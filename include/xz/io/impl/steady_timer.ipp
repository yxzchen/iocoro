#pragma once

#include <xz/io/steady_timer.hpp>

namespace xz::io {

void steady_timer::async_wait_op::start_operation() {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration_);

  timer_.timer_handle_ = timer_.get_executor().schedule_timer(ms, [this]() {
    timer_.timer_handle_.reset();
    complete({});
  });
}

}  // namespace xz::io
