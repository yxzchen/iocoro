#include <iocoro/result.hpp>
#include <iocoro/with_timeout.hpp>

#include <chrono>

namespace {

using namespace std::chrono_literals;

[[maybe_unused]] auto smoke_int() -> iocoro::awaitable<int> {
  co_return 1;
}

[[maybe_unused]] auto smoke_result_int() -> iocoro::awaitable<iocoro::result<int>> {
  co_return iocoro::result<int>{42};
}

[[maybe_unused]] auto compile_only_smoke() -> iocoro::awaitable<void> {
  auto [index, any_result] = co_await iocoro::when_any_cancel_join(smoke_int(), smoke_int());
  (void)index;
  (void)any_result;

  auto timed_result = co_await iocoro::with_timeout(smoke_result_int(), 1ms);
  (void)timed_result;
}

}  // namespace
