// switch_executor.cpp
//
// Purpose:
//   Demonstrate switching coroutine scheduling between different executors.
//
// Preconditions:
//   - Starts on an IO-capable executor (io_context).
//   - After switching to a non-IO executor (thread_pool), do NOT perform IO-only operations
//     (e.g. co_sleep) until switched back to an IO-capable executor.
//
// Notes (development stage):
//   - Switching semantics and edge cases may change as the project evolves.

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {

auto co_main(iocoro::io_context& ctx, iocoro::any_executor io_ex, iocoro::any_executor cpu_ex)
  -> iocoro::awaitable<void> {
  std::cout << "switch_executor: start on thread " << std::this_thread::get_id() << "\n";

  co_await iocoro::this_coro::switch_to(cpu_ex);
  std::cout << "switch_executor: on thread_pool thread " << std::this_thread::get_id() << "\n";

  std::uint64_t sum = 0;
  for (std::uint64_t i = 0; i < 5'000'000; ++i) {
    sum += i;
  }
  std::cout << "switch_executor: cpu work done, sum=" << sum << "\n";

  co_await iocoro::this_coro::switch_to(io_ex);
  std::cout << "switch_executor: back on io_context thread " << std::this_thread::get_id() << "\n";

  co_await iocoro::co_sleep(20ms);
  std::cout << "switch_executor: done\n";
  ctx.stop();
}

}  // namespace

int main() {
  iocoro::io_context ctx;
  iocoro::thread_pool pool{1};

  auto io_ex = iocoro::any_executor{ctx.get_executor()};
  auto cpu_ex = iocoro::any_executor{pool.get_executor()};

  auto guard = iocoro::make_work_guard(ctx);
  iocoro::co_spawn(io_ex, co_main(ctx, io_ex, cpu_ex), iocoro::detached);
  ctx.run();

  pool.stop();
  pool.join();
  return 0;
}
