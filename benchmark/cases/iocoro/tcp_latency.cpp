#include <iocoro/iocoro.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

using iocoro::ip::tcp;

struct bench_state {
  iocoro::io_context* ctx = nullptr;
  std::atomic<int> remaining_sessions{0};
  std::atomic<bool> failed{false};
  int msgs_per_session = 0;
  std::vector<std::byte> payload{};
  std::mutex latency_mtx{};
  std::vector<double> latencies_us{};
};

auto percentile_sorted(std::vector<double> const& sorted, double q) -> double {
  if (sorted.empty()) {
    return 0.0;
  }
  if (q <= 0.0) {
    return sorted.front();
  }
  if (q >= 1.0) {
    return sorted.back();
  }
  auto const idx = static_cast<std::size_t>(
    std::ceil(q * static_cast<double>(sorted.size() - 1)));
  return sorted[idx];
}

inline void append_latencies(bench_state* st, std::vector<double>& local) {
  std::scoped_lock lk{st->latency_mtx};
  st->latencies_us.insert(st->latencies_us.end(), local.begin(), local.end());
}

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

auto echo_session(tcp::socket socket, bench_state* st) -> iocoro::awaitable<void> {
  std::vector<std::byte> recv_buf(st->payload.size());
  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto r = co_await iocoro::io::async_read(socket, iocoro::net::buffer(recv_buf));
    if (!r) {
      fail_and_stop(st, "iocoro_tcp_latency: server read failed: " + r.error().message());
      co_return;
    }
    auto w = co_await iocoro::io::async_write(socket, iocoro::net::buffer(recv_buf));
    if (!w) {
      fail_and_stop(st, "iocoro_tcp_latency: server write failed: " + w.error().message());
      co_return;
    }
  }
}

auto accept_loop(tcp::acceptor& acceptor, int sessions, bench_state* st)
  -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;
  for (int i = 0; i < sessions; ++i) {
    auto accepted = co_await acceptor.async_accept();
    if (!accepted) {
      fail_and_stop(st, "iocoro_tcp_latency: accept failed: " + accepted.error().message());
      co_return;
    }
    iocoro::co_spawn(ex, echo_session(std::move(*accepted), st), iocoro::detached);
  }
}

auto client_session(iocoro::io_context& ctx, tcp::endpoint ep, bench_state* st)
  -> iocoro::awaitable<void> {
  tcp::socket socket{ctx};
  auto cr = co_await socket.async_connect(ep);
  if (!cr) {
    fail_and_stop(st, "iocoro_tcp_latency: connect failed: " + cr.error().message());
    co_return;
  }

  std::vector<std::byte> response(st->payload.size());
  std::vector<double> local_latencies{};
  local_latencies.reserve(static_cast<std::size_t>(st->msgs_per_session));

  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto const start = std::chrono::steady_clock::now();
    auto w = co_await iocoro::io::async_write(socket, iocoro::net::buffer(st->payload));
    if (!w) {
      fail_and_stop(st, "iocoro_tcp_latency: client write failed: " + w.error().message());
      co_return;
    }
    auto r = co_await iocoro::io::async_read(socket, iocoro::net::buffer(response));
    if (!r) {
      fail_and_stop(st, "iocoro_tcp_latency: client read failed: " + r.error().message());
      co_return;
    }
    auto const end = std::chrono::steady_clock::now();
    auto const us = std::chrono::duration<double, std::micro>(end - start).count();
    local_latencies.push_back(us);
  }

  append_latencies(st, local_latencies);
  mark_done(st);
}

}  // namespace

int main(int argc, char* argv[]) {
  int sessions = 1;
  int msgs = 1;
  std::size_t msg_bytes = 64;
  if (argc >= 3) {
    sessions = std::stoi(argv[1]);
    msgs = std::stoi(argv[2]);
  }
  if (argc >= 4) {
    msg_bytes = static_cast<std::size_t>(std::stoull(argv[3]));
  }
  if (sessions <= 0) {
    std::cerr << "iocoro_tcp_latency: sessions must be > 0\n";
    return 1;
  }
  if (msgs <= 0) {
    std::cerr << "iocoro_tcp_latency: msgs must be > 0\n";
    return 1;
  }
  if (msg_bytes == 0) {
    std::cerr << "iocoro_tcp_latency: msg_bytes must be > 0\n";
    return 1;
  }

  iocoro::io_context ctx;

  tcp::acceptor acceptor{ctx};
  auto listen_ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), 0};
  auto lr = acceptor.listen(listen_ep);
  if (!lr) {
    std::cerr << "iocoro_tcp_latency: listen failed: " << lr.error().message() << "\n";
    return 1;
  }

  auto ep_r = acceptor.local_endpoint();
  if (!ep_r) {
    std::cerr << "iocoro_tcp_latency: local_endpoint failed: " << ep_r.error().message() << "\n";
    return 1;
  }

  bench_state st{};
  st.ctx = &ctx;
  st.remaining_sessions.store(sessions, std::memory_order_release);
  st.msgs_per_session = msgs;
  st.payload.assign(msg_bytes, std::byte{0x78});
  st.latencies_us.reserve(static_cast<std::size_t>(sessions) * static_cast<std::size_t>(msgs));

  auto ex = ctx.get_executor();
  auto guard = iocoro::make_work_guard(ctx);

  iocoro::co_spawn(ex, accept_loop(acceptor, sessions, &st), iocoro::detached);
  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex, client_session(ctx, *ep_r, &st), iocoro::detached);
  }

  auto const expected_samples =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(msgs);

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  std::vector<double> samples{};
  {
    std::scoped_lock lk{st.latency_mtx};
    samples = st.latencies_us;
  }
  std::sort(samples.begin(), samples.end());

  double total_us = 0.0;
  for (auto const v : samples) {
    total_us += v;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const sample_count = static_cast<std::uint64_t>(samples.size());

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }
  if (st.remaining_sessions.load(std::memory_order_acquire) != 0) {
    std::cerr << "iocoro_tcp_latency: incomplete run (remaining_sessions="
              << st.remaining_sessions.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }
  if (sample_count != expected_samples) {
    std::cerr << "iocoro_tcp_latency: sample mismatch (expected=" << expected_samples
              << ", got=" << sample_count << ")\n";
    return 1;
  }

  auto const p50_us = percentile_sorted(samples, 0.50);
  auto const p95_us = percentile_sorted(samples, 0.95);
  auto const p99_us = percentile_sorted(samples, 0.99);
  auto const avg_us = sample_count > 0 ? total_us / static_cast<double>(sample_count) : 0.0;
  auto const rps = elapsed_s > 0.0 ? static_cast<double>(sample_count) / elapsed_s : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "iocoro_tcp_latency"
            << " listen=" << ep_r->to_string()
            << " sessions=" << sessions
            << " msgs=" << msgs
            << " msg_bytes=" << msg_bytes
            << " samples=" << sample_count
            << " expected_samples=" << expected_samples
            << " elapsed_s=" << elapsed_s
            << " rps=" << rps
            << " avg_us=" << avg_us
            << " p50_us=" << p50_us
            << " p95_us=" << p95_us
            << " p99_us=" << p99_us << "\n";

  return 0;
}
