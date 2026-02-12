#include <iocoro/iocoro.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using iocoro::ip::tcp;

struct bench_state {
  iocoro::io_context* ctx = nullptr;
  std::atomic<int> remaining_events{0};
  std::atomic<bool> failed{false};
  std::uint64_t bytes_per_session = 0;
  std::size_t chunk_bytes = 0;
};

inline void mark_done(bench_state* st) {
  if (st->remaining_events.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    st->ctx->stop();
  }
}

inline void fail_and_stop(bench_state* st, std::string message) {
  if (!st->failed.exchange(true, std::memory_order_acq_rel)) {
    std::cerr << message << "\n";
  }
  st->ctx->stop();
}

auto server_session(tcp::socket socket, bench_state* st) -> iocoro::awaitable<void> {
  std::vector<std::byte> read_buf(st->chunk_bytes);
  auto remaining = st->bytes_per_session;

  while (remaining > 0) {
    auto const to_read =
      static_cast<std::size_t>(std::min<std::uint64_t>(remaining, read_buf.size()));
    auto r = co_await socket.async_read_some(iocoro::net::buffer(read_buf.data(), to_read));
    if (!r || *r == 0) {
      if (!r) {
        fail_and_stop(st, "iocoro_tcp_throughput: server read failed: " + r.error().message());
      } else {
        fail_and_stop(st, "iocoro_tcp_throughput: server read returned 0");
      }
      co_return;
    }
    remaining -= static_cast<std::uint64_t>(*r);
  }

  mark_done(st);
}

auto accept_loop(tcp::acceptor& acceptor, int sessions, bench_state* st) -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;
  for (int i = 0; i < sessions; ++i) {
    auto accepted = co_await acceptor.async_accept();
    if (!accepted) {
      fail_and_stop(st, "iocoro_tcp_throughput: accept failed: " + accepted.error().message());
      co_return;
    }
    iocoro::co_spawn(ex, server_session(std::move(*accepted), st), iocoro::detached);
  }
}

auto client_session(iocoro::io_context& ctx, tcp::endpoint ep, bench_state* st)
  -> iocoro::awaitable<void> {
  tcp::socket socket{ctx};
  auto cr = co_await socket.async_connect(ep);
  if (!cr) {
    fail_and_stop(st, "iocoro_tcp_throughput: connect failed: " + cr.error().message());
    co_return;
  }

  std::vector<std::byte> payload(st->chunk_bytes, std::byte{0x78});
  auto remaining = st->bytes_per_session;
  while (remaining > 0) {
    auto const to_send =
      static_cast<std::size_t>(std::min<std::uint64_t>(remaining, payload.size()));
    auto w = co_await iocoro::io::async_write(socket, iocoro::net::buffer(payload.data(), to_send));
    if (!w || *w == 0) {
      if (!w) {
        fail_and_stop(st, "iocoro_tcp_throughput: client write failed: " + w.error().message());
      } else {
        fail_and_stop(st, "iocoro_tcp_throughput: client write returned 0");
      }
      co_return;
    }
    remaining -= static_cast<std::uint64_t>(*w);
  }

  mark_done(st);
}

}  // namespace

int main(int argc, char* argv[]) {
  int sessions = 1;
  std::uint64_t bytes_per_session = 8ULL * 1024ULL * 1024ULL;
  std::size_t chunk_bytes = 16ULL * 1024ULL;
  if (argc >= 2) {
    sessions = std::stoi(argv[1]);
  }
  if (argc >= 3) {
    bytes_per_session = static_cast<std::uint64_t>(std::stoull(argv[2]));
  }
  if (argc >= 4) {
    chunk_bytes = static_cast<std::size_t>(std::stoull(argv[3]));
  }
  if (sessions <= 0) {
    std::cerr << "iocoro_tcp_throughput: sessions must be > 0\n";
    return 1;
  }
  if (bytes_per_session == 0) {
    std::cerr << "iocoro_tcp_throughput: bytes_per_session must be > 0\n";
    return 1;
  }
  if (chunk_bytes == 0) {
    std::cerr << "iocoro_tcp_throughput: chunk_bytes must be > 0\n";
    return 1;
  }

  iocoro::io_context ctx;

  tcp::acceptor acceptor{ctx};
  auto listen_ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), 0};
  auto lr = acceptor.listen(listen_ep);
  if (!lr) {
    std::cerr << "iocoro_tcp_throughput: listen failed: " << lr.error().message() << "\n";
    return 1;
  }

  auto ep_r = acceptor.local_endpoint();
  if (!ep_r) {
    std::cerr << "iocoro_tcp_throughput: local_endpoint failed: " << ep_r.error().message() << "\n";
    return 1;
  }

  bench_state st{};
  st.ctx = &ctx;
  st.bytes_per_session = bytes_per_session;
  st.chunk_bytes = chunk_bytes;
  st.remaining_events.store(sessions * 2, std::memory_order_release);

  auto ex = ctx.get_executor();
  auto guard = iocoro::make_work_guard(ctx);

  iocoro::co_spawn(ex, accept_loop(acceptor, sessions, &st), iocoro::detached);
  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex, client_session(ctx, *ep_r, &st), iocoro::detached);
  }

  auto const total_bytes =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(bytes_per_session);

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }
  if (st.remaining_events.load(std::memory_order_acquire) != 0) {
    std::cerr << "iocoro_tcp_throughput: incomplete run (remaining_events="
              << st.remaining_events.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const throughput_mib_s =
    elapsed_s > 0.0 ? (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / elapsed_s : 0.0;
  auto const avg_session_ms =
    sessions > 0 && elapsed_s > 0.0 ? (elapsed_s * 1000.0) / static_cast<double>(sessions) : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "iocoro_tcp_throughput"
            << " listen=" << ep_r->to_string()
            << " sessions=" << sessions
            << " bytes_per_session=" << bytes_per_session
            << " chunk_bytes=" << chunk_bytes
            << " total_bytes=" << total_bytes
            << " elapsed_s=" << elapsed_s
            << " throughput_mib_s=" << throughput_mib_s
            << " avg_session_ms=" << avg_session_ms << "\n";

  return 0;
}
