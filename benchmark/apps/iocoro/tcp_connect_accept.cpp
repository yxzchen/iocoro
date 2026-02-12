#include <iocoro/iocoro.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

using iocoro::ip::tcp;

struct bench_state {
  iocoro::io_context* ctx = nullptr;
  std::atomic<int> remaining_events{0};
};

inline void mark_done(bench_state* st) {
  if (st->remaining_events.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    st->ctx->stop();
  }
}

auto accept_loop(tcp::acceptor& acceptor, int connections, bench_state* st) -> iocoro::awaitable<void> {
  for (int i = 0; i < connections; ++i) {
    auto accepted = co_await acceptor.async_accept();
    if (!accepted) {
      st->ctx->stop();
      co_return;
    }
    mark_done(st);
  }
}

auto client_once(iocoro::io_context& ctx, tcp::endpoint ep, bench_state* st) -> iocoro::awaitable<void> {
  tcp::socket socket{ctx};
  auto cr = co_await socket.async_connect(ep);
  if (!cr) {
    st->ctx->stop();
    co_return;
  }
  mark_done(st);
}

}  // namespace

int main(int argc, char* argv[]) {
  int connections = 1000;
  if (argc >= 2) {
    connections = std::stoi(argv[1]);
  }
  if (connections <= 0) {
    std::cerr << "iocoro_tcp_connect_accept: connections must be > 0\n";
    return 1;
  }

  iocoro::io_context ctx;

  tcp::acceptor acceptor{ctx};
  auto listen_ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), 0};
  auto lr = acceptor.listen(listen_ep);
  if (!lr) {
    std::cerr << "iocoro_tcp_connect_accept: listen failed: " << lr.error().message() << "\n";
    return 1;
  }

  auto ep_r = acceptor.local_endpoint();
  if (!ep_r) {
    std::cerr << "iocoro_tcp_connect_accept: local_endpoint failed: " << ep_r.error().message() << "\n";
    return 1;
  }

  bench_state st{};
  st.ctx = &ctx;
  st.remaining_events.store(connections * 2, std::memory_order_release);

  auto ex = ctx.get_executor();
  auto guard = iocoro::make_work_guard(ctx);

  iocoro::co_spawn(ex, accept_loop(acceptor, connections, &st), iocoro::detached);
  for (int i = 0; i < connections; ++i) {
    iocoro::co_spawn(ex, client_once(ctx, *ep_r, &st), iocoro::detached);
  }

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const cps = elapsed_s > 0.0 ? static_cast<double>(connections) / elapsed_s : 0.0;
  auto const avg_us = connections > 0 && elapsed_s > 0.0
                        ? (elapsed_s * 1'000'000.0) / static_cast<double>(connections)
                        : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "iocoro_tcp_connect_accept"
            << " listen=" << ep_r->to_string()
            << " connections=" << connections
            << " elapsed_s=" << elapsed_s
            << " cps=" << cps
            << " avg_us=" << avg_us << "\n";

  return 0;
}
