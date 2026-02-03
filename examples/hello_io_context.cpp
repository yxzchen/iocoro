// hello_io_context.cpp
//
// Purpose:
//   Minimal runnable example showing io_context + co_spawn + co_sleep.
//
// Preconditions:
//   - Requires an IO-capable executor (provided by iocoro::io_context).
//
// Notes (development stage):
//   - This example demonstrates usage only. Semantics and APIs may change.

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {

auto demo() -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;
  (void)ex;

  std::cout << "hello_io_context: start on thread " << std::this_thread::get_id() << "\n";
  co_await iocoro::co_sleep(50ms);
  std::cout << "hello_io_context: after co_sleep on thread " << std::this_thread::get_id() << "\n";
}

}  // namespace

int main() {
  iocoro::io_context ctx;

  iocoro::co_spawn(ctx.get_executor(), demo(), iocoro::detached);

  ctx.run();
  return 0;
}
