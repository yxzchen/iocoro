#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace net = boost::asio;
using net::awaitable;
using net::co_spawn;
using net::detached;
using net::use_awaitable;

namespace {

struct bench_state {
  net::io_context* ioc = nullptr;
  std::atomic<int> remaining_sessions{0};
  int waits_per_session = 0;
};

inline void mark_done(bench_state* st) {
  if (st->remaining_sessions.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    st->ioc->stop();
  }
}

auto timer_session(net::any_io_executor ex, bench_state* st) -> awaitable<void> {
  net::steady_timer timer{ex};
  for (int i = 0; i < st->waits_per_session; ++i) {
    timer.expires_after(std::chrono::milliseconds{0});
    boost::system::error_code ec;
    (void)co_await timer.async_wait(net::redirect_error(use_awaitable, ec));
    if (ec) {
      st->ioc->stop();
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
    std::cerr << "asio_timer_churn: sessions must be > 0\n";
    return 1;
  }
  if (waits <= 0) {
    std::cerr << "asio_timer_churn: waits must be > 0\n";
    return 1;
  }

  net::io_context ioc;

  bench_state st{};
  st.ioc = &ioc;
  st.waits_per_session = waits;
  st.remaining_sessions.store(sessions, std::memory_order_release);

  auto ex = ioc.get_executor();
  for (int i = 0; i < sessions; ++i) {
    co_spawn(ex, timer_session(ex, &st), detached);
  }

  auto const total_waits = static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(waits);

  auto const start = std::chrono::steady_clock::now();
  ioc.run();
  auto const end = std::chrono::steady_clock::now();

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const ops_s = elapsed_s > 0.0 ? static_cast<double>(total_waits) / elapsed_s : 0.0;
  auto const avg_us = total_waits > 0 && elapsed_s > 0.0
                        ? (elapsed_s * 1'000'000.0) / static_cast<double>(total_waits)
                        : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "asio_timer_churn"
            << " sessions=" << sessions
            << " waits=" << waits
            << " total_waits=" << total_waits
            << " elapsed_s=" << elapsed_s
            << " ops_s=" << ops_s
            << " avg_us=" << avg_us << "\n";

  return 0;
}
