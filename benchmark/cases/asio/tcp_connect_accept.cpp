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
using net::ip::tcp;
using net::use_awaitable;

namespace {

struct bench_state {
  net::io_context* ioc = nullptr;
  std::atomic<int> remaining_events{0};
  std::atomic<bool> failed{false};
};

inline void mark_done(bench_state* st) {
  if (st->remaining_events.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    st->ioc->stop();
  }
}

inline void fail_and_stop(bench_state* st, std::string message) {
  if (!st->failed.exchange(true, std::memory_order_acq_rel)) {
    std::cerr << message << "\n";
  }
  st->ioc->stop();
}

auto accept_loop(tcp::acceptor& acceptor, int connections, bench_state* st) -> awaitable<void> {
  for (int i = 0; i < connections; ++i) {
    boost::system::error_code ec;
    auto socket = co_await acceptor.async_accept(net::redirect_error(use_awaitable, ec));
    if (ec) {
      fail_and_stop(st, "asio_tcp_connect_accept: accept failed: " + ec.message());
      co_return;
    }
    mark_done(st);
  }
}

auto client_once(net::io_context& ioc, tcp::endpoint ep, bench_state* st) -> awaitable<void> {
  auto ex = co_await net::this_coro::executor;
  tcp::socket socket{ex};

  boost::system::error_code ec;
  co_await socket.async_connect(ep, net::redirect_error(use_awaitable, ec));
  if (ec) {
    fail_and_stop(st, "asio_tcp_connect_accept: connect failed: " + ec.message());
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
    std::cerr << "asio_tcp_connect_accept: connections must be > 0\n";
    return 1;
  }

  net::io_context ioc;

  tcp::acceptor acceptor{ioc, tcp::endpoint{net::ip::make_address_v4("127.0.0.1"), 0}};
  auto const listen_ep = acceptor.local_endpoint();

  bench_state st{};
  st.ioc = &ioc;
  st.remaining_events.store(connections * 2, std::memory_order_release);

  co_spawn(ioc, accept_loop(acceptor, connections, &st), detached);
  for (int i = 0; i < connections; ++i) {
    co_spawn(ioc, client_once(ioc, listen_ep, &st), detached);
  }

  auto const start = std::chrono::steady_clock::now();
  ioc.run();
  auto const end = std::chrono::steady_clock::now();

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }
  if (st.remaining_events.load(std::memory_order_acquire) != 0) {
    std::cerr << "asio_tcp_connect_accept: incomplete run (remaining_events="
              << st.remaining_events.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const cps = elapsed_s > 0.0 ? static_cast<double>(connections) / elapsed_s : 0.0;
  auto const avg_us = connections > 0 && elapsed_s > 0.0
                        ? (elapsed_s * 1'000'000.0) / static_cast<double>(connections)
                        : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "asio_tcp_connect_accept"
            << " listen=" << listen_ep.address().to_string() << ":" << listen_ep.port()
            << " connections=" << connections
            << " elapsed_s=" << elapsed_s
            << " cps=" << cps
            << " avg_us=" << avg_us << "\n";

  return 0;
}
