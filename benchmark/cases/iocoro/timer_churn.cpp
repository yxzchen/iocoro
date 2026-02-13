#include <iocoro/iocoro.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

struct bench_state {
  iocoro::io_context* ctx = nullptr;
  std::atomic<int> remaining_sessions{0};
  std::atomic<bool> failed{false};
  int waits_per_session = 0;
};

inline void mark_done(bench_state* st) {
  if (st->remaining_sessions.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    st->ctx->stop();
  }
}

inline void fail_and_stop(bench_state* st, std::string message) {
  if (!st->failed.exchange(true, std::memory_order_acq_rel)) {
    std::cerr << message << "\n";
  }
  st->ctx->stop();
}

auto timer_session(iocoro::any_io_executor ex, bench_state* st) -> iocoro::awaitable<void> {
  iocoro::steady_timer timer{ex};
  for (int i = 0; i < st->waits_per_session; ++i) {
    timer.expires_after(std::chrono::milliseconds{0});
    auto r = co_await timer.async_wait(iocoro::use_awaitable);
    if (!r) {
      fail_and_stop(st, "iocoro_timer_churn: timer wait failed: " + r.error().message());
      co_return;
    }
  }
  mark_done(st);
}

}  // namespace

int main(int argc, char* argv[]) {
  int sessions = 1;
  int waits = 1;
  if (argc >= 3) {
    sessions = std::stoi(argv[1]);
    waits = std::stoi(argv[2]);
  }
  if (sessions <= 0) {
    std::cerr << "iocoro_timer_churn: sessions must be > 0\n";
    return 1;
  }
  if (waits <= 0) {
    std::cerr << "iocoro_timer_churn: waits must be > 0\n";
    return 1;
  }

  iocoro::io_context ctx;
  auto ex = ctx.get_executor();
  auto guard = iocoro::make_work_guard(ctx);

  bench_state st{};
  st.ctx = &ctx;
  st.waits_per_session = waits;
  st.remaining_sessions.store(sessions, std::memory_order_release);

  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex, timer_session(ex, &st), iocoro::detached);
  }

  auto const total_waits = static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(waits);

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }
  if (st.remaining_sessions.load(std::memory_order_acquire) != 0) {
    std::cerr << "iocoro_timer_churn: incomplete run (remaining_sessions="
              << st.remaining_sessions.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const ops_s = elapsed_s > 0.0 ? static_cast<double>(total_waits) / elapsed_s : 0.0;
  auto const avg_us = total_waits > 0 && elapsed_s > 0.0
                        ? (elapsed_s * 1'000'000.0) / static_cast<double>(total_waits)
                        : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "iocoro_timer_churn"
            << " sessions=" << sessions << " waits=" << waits << " total_waits=" << total_waits
            << " elapsed_s=" << elapsed_s << " ops_s=" << ops_s << " avg_us=" << avg_us << "\n";

  return 0;
}
