// iocoro_tcp_roundtrip.cpp
//
// Single-process TCP roundtrip benchmark using real sockets:
// - Start a TCP acceptor on 127.0.0.1:0 (ephemeral port)
// - Spawn N client sessions that connect and perform M request/response roundtrips
//
// Notes:
// - Development-stage benchmark only; not representative of real-world performance.

#include <iocoro/iocoro.hpp>
#include <iocoro/ip.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>

namespace {

using iocoro::ip::tcp;

struct bench_state {
  iocoro::io_context* ctx = nullptr;
  std::atomic<int> remaining_sessions{0};
  int msgs_per_session = 0;
  std::string msg{};
};

auto echo_session(tcp::socket socket, bench_state* st) -> iocoro::awaitable<void> {
  std::string buffer(4096, '\0');
  auto buf = iocoro::net::buffer(buffer);

  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto r = co_await iocoro::io::async_read_until(socket, buf, '\n', 0);
    if (!r) {
      co_return;
    }

    auto const n = *r;
    auto w = co_await iocoro::io::async_write(socket, iocoro::net::buffer(buffer.data(), n));
    if (!w) {
      co_return;
    }
  }
}

auto accept_loop(tcp::acceptor& acceptor, int sessions, bench_state* st) -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;

  for (int i = 0; i < sessions; ++i) {
    auto accepted = co_await acceptor.async_accept();
    if (!accepted) {
      st->ctx->stop();
      co_return;
    }
    iocoro::co_spawn(ex, echo_session(std::move(*accepted), st), iocoro::detached);
  }
}

auto client_session(iocoro::io_context& ctx, tcp::endpoint ep, bench_state* st) -> iocoro::awaitable<void> {
  tcp::socket socket{ctx};
  auto cr = co_await socket.async_connect(ep);
  if (!cr) {
    if (st->remaining_sessions.fetch_sub(1) == 1) {
      ctx.stop();
    }
    co_return;
  }

  std::string buffer(4096, '\0');
  auto buf = iocoro::net::buffer(buffer);

  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto w = co_await iocoro::io::async_write(socket, iocoro::net::buffer(st->msg));
    if (!w) {
      break;
    }

    auto r = co_await iocoro::io::async_read_until(socket, buf, '\n', 0);
    if (!r) {
      break;
    }
  }

  if (st->remaining_sessions.fetch_sub(1) == 1) {
    ctx.stop();
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  int sessions = 1;
  int msgs = 1;
  if (argc == 3) {
    sessions = std::stoi(argv[1]);
    msgs = std::stoi(argv[2]);
  }

  iocoro::io_context ctx;

  tcp::acceptor acceptor{ctx};
  auto listen_ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), 0};
  auto lr = acceptor.listen(listen_ep);
  if (!lr) {
    std::cerr << "iocoro_tcp_roundtrip: listen failed: " << lr.error().message() << "\n";
    return 1;
  }

  auto ep_r = acceptor.local_endpoint();
  if (!ep_r) {
    std::cerr << "iocoro_tcp_roundtrip: local_endpoint failed: " << ep_r.error().message() << "\n";
    return 1;
  }

  bench_state st{};
  st.ctx = &ctx;
  st.remaining_sessions.store(sessions);
  st.msgs_per_session = msgs;
  st.msg = "Some message\n";

  auto ex = ctx.get_executor();
  auto guard = iocoro::make_work_guard(ctx);

  iocoro::co_spawn(ex, accept_loop(acceptor, sessions, &st), iocoro::detached);
  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex, client_session(ctx, *ep_r, &st), iocoro::detached);
  }

  auto const msg_bytes = st.msg.size();
  auto const total_roundtrips = static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(msgs);
  auto const total_tx_bytes = total_roundtrips * msg_bytes;
  auto const total_rx_bytes = total_roundtrips * msg_bytes;

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const rps = elapsed_s > 0.0 ? static_cast<double>(total_roundtrips) / elapsed_s : 0.0;
  auto const avg_us = total_roundtrips > 0 && elapsed_s > 0.0
                        ? (elapsed_s * 1'000'000.0) / static_cast<double>(total_roundtrips)
                        : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "iocoro_tcp_roundtrip"
            << " listen=" << ep_r->to_string()
            << " sessions=" << sessions
            << " msgs=" << msgs
            << " msg_bytes=" << msg_bytes
            << " roundtrips=" << total_roundtrips
            << " tx_bytes=" << total_tx_bytes
            << " rx_bytes=" << total_rx_bytes
            << " elapsed_s=" << elapsed_s
            << " rps=" << rps
            << " avg_us=" << avg_us
            << "\n";

  return 0;
}

