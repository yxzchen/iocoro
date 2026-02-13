#include <boost/asio.hpp>

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

namespace net = boost::asio;
using net::awaitable;
using net::co_spawn;
using net::detached;
using net::use_awaitable;
using net::ip::tcp;

namespace {

struct bench_state {
  net::io_context* ioc = nullptr;
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
  auto const idx = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size() - 1)));
  return sorted[idx];
}

inline void append_latencies(bench_state* st, std::vector<double>& local) {
  std::scoped_lock lk{st->latency_mtx};
  st->latencies_us.insert(st->latencies_us.end(), local.begin(), local.end());
}

inline void mark_done(bench_state* st) {
  if (st->remaining_sessions.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    st->ioc->stop();
  }
}

inline void fail_and_stop(bench_state* st, std::string message) {
  if (!st->failed.exchange(true, std::memory_order_acq_rel)) {
    std::cerr << message << "\n";
  }
  st->ioc->stop();
}

auto echo_session(tcp::socket socket, bench_state* st) -> awaitable<void> {
  std::vector<std::byte> recv_buf(st->payload.size());
  for (int i = 0; i < st->msgs_per_session; ++i) {
    boost::system::error_code ec;
    auto n = co_await net::async_read(socket, net::buffer(recv_buf),
                                      net::redirect_error(use_awaitable, ec));
    if (ec || n != recv_buf.size()) {
      if (ec) {
        fail_and_stop(st, "asio_tcp_latency: server read failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_tcp_latency: server read size mismatch");
      }
      co_return;
    }
    n = co_await net::async_write(socket, net::buffer(recv_buf),
                                  net::redirect_error(use_awaitable, ec));
    if (ec || n != recv_buf.size()) {
      if (ec) {
        fail_and_stop(st, "asio_tcp_latency: server write failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_tcp_latency: server write size mismatch");
      }
      co_return;
    }
  }
}

auto accept_loop(tcp::acceptor& acceptor, int sessions, bench_state* st) -> awaitable<void> {
  auto ex = co_await net::this_coro::executor;
  for (int i = 0; i < sessions; ++i) {
    boost::system::error_code ec;
    auto socket = co_await acceptor.async_accept(net::redirect_error(use_awaitable, ec));
    if (ec) {
      fail_and_stop(st, "asio_tcp_latency: accept failed: " + ec.message());
      co_return;
    }
    co_spawn(ex, echo_session(std::move(socket), st), detached);
  }
}

auto client_session(net::io_context& ioc, tcp::endpoint ep, bench_state* st) -> awaitable<void> {
  auto ex = co_await net::this_coro::executor;
  tcp::socket socket{ex};

  boost::system::error_code ec;
  co_await socket.async_connect(ep, net::redirect_error(use_awaitable, ec));
  if (ec) {
    fail_and_stop(st, "asio_tcp_latency: connect failed: " + ec.message());
    co_return;
  }

  std::vector<std::byte> response(st->payload.size());
  std::vector<double> local_latencies{};
  local_latencies.reserve(static_cast<std::size_t>(st->msgs_per_session));

  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto const start = std::chrono::steady_clock::now();
    auto n = co_await net::async_write(socket, net::buffer(st->payload),
                                       net::redirect_error(use_awaitable, ec));
    if (ec || n != st->payload.size()) {
      if (ec) {
        fail_and_stop(st, "asio_tcp_latency: client write failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_tcp_latency: client write size mismatch");
      }
      co_return;
    }
    n = co_await net::async_read(socket, net::buffer(response),
                                 net::redirect_error(use_awaitable, ec));
    if (ec || n != response.size()) {
      if (ec) {
        fail_and_stop(st, "asio_tcp_latency: client read failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_tcp_latency: client read size mismatch");
      }
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
    std::cerr << "asio_tcp_latency: sessions must be > 0\n";
    return 1;
  }
  if (msgs <= 0) {
    std::cerr << "asio_tcp_latency: msgs must be > 0\n";
    return 1;
  }
  if (msg_bytes == 0) {
    std::cerr << "asio_tcp_latency: msg_bytes must be > 0\n";
    return 1;
  }

  net::io_context ioc;
  tcp::acceptor acceptor{ioc, tcp::endpoint{net::ip::make_address_v4("127.0.0.1"), 0}};
  auto const listen_ep = acceptor.local_endpoint();

  bench_state st{};
  st.ioc = &ioc;
  st.remaining_sessions.store(sessions, std::memory_order_release);
  st.msgs_per_session = msgs;
  st.payload.assign(msg_bytes, std::byte{0x78});
  st.latencies_us.reserve(static_cast<std::size_t>(sessions) * static_cast<std::size_t>(msgs));

  co_spawn(ioc, accept_loop(acceptor, sessions, &st), detached);
  for (int i = 0; i < sessions; ++i) {
    co_spawn(ioc, client_session(ioc, listen_ep, &st), detached);
  }

  auto const expected_samples =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(msgs);

  auto const start = std::chrono::steady_clock::now();
  ioc.run();
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
    std::cerr << "asio_tcp_latency: incomplete run (remaining_sessions="
              << st.remaining_sessions.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }
  if (sample_count != expected_samples) {
    std::cerr << "asio_tcp_latency: sample mismatch (expected=" << expected_samples
              << ", got=" << sample_count << ")\n";
    return 1;
  }

  auto const p50_us = percentile_sorted(samples, 0.50);
  auto const p95_us = percentile_sorted(samples, 0.95);
  auto const p99_us = percentile_sorted(samples, 0.99);
  auto const avg_us = sample_count > 0 ? total_us / static_cast<double>(sample_count) : 0.0;
  auto const rps = elapsed_s > 0.0 ? static_cast<double>(sample_count) / elapsed_s : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "asio_tcp_latency"
            << " listen=" << listen_ep.address().to_string() << ":" << listen_ep.port()
            << " sessions=" << sessions << " msgs=" << msgs << " msg_bytes=" << msg_bytes
            << " samples=" << sample_count << " expected_samples=" << expected_samples
            << " elapsed_s=" << elapsed_s << " rps=" << rps << " avg_us=" << avg_us
            << " p50_us=" << p50_us << " p95_us=" << p95_us << " p99_us=" << p99_us << "\n";

  return 0;
}
