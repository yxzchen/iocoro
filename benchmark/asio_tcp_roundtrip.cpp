#include <boost/asio.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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
  int msgs_per_session = 0;
  std::size_t io_buffer_size = 4096;
  std::string msg{};
};

auto echo_session(tcp::socket socket, bench_state* st) -> awaitable<void> {
  std::string buffer;
  buffer.reserve(st->io_buffer_size);
  auto dbuf = net::dynamic_buffer(buffer, st->io_buffer_size);

  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto n = co_await net::async_read_until(socket, dbuf, '\n', use_awaitable);
    co_await net::async_write(socket, net::buffer(buffer.data(), n), use_awaitable);
    dbuf.consume(n);
  }
}

auto accept_loop(tcp::acceptor& acceptor, int sessions, bench_state* st) -> awaitable<void> {
  auto ex = co_await net::this_coro::executor;
  for (int i = 0; i < sessions; ++i) {
    tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
    co_spawn(ex, echo_session(std::move(socket), st), detached);
  }
}

auto client_session(net::io_context& ioc, tcp::endpoint ep, bench_state* st) -> awaitable<void> {
  auto ex = co_await net::this_coro::executor;

  tcp::socket socket{ex};
  co_await socket.async_connect(ep, use_awaitable);

  std::string buffer;
  buffer.reserve(st->io_buffer_size);
  auto dbuf = net::dynamic_buffer(buffer, st->io_buffer_size);

  for (int i = 0; i < st->msgs_per_session; ++i) {
    co_await net::async_write(socket, net::buffer(st->msg), use_awaitable);
    auto n = co_await net::async_read_until(socket, dbuf, '\n', use_awaitable);
    dbuf.consume(n);
  }

  if (st->remaining_sessions.fetch_sub(1) == 1) {
    ioc.stop();
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  int sessions = 1;
  int msgs = 1;
  std::size_t msg_bytes = 13;
  if (argc >= 3) {
    sessions = std::stoi(argv[1]);
    msgs = std::stoi(argv[2]);
  }
  if (argc >= 4) {
    msg_bytes = static_cast<std::size_t>(std::stoul(argv[3]));
  }
  if (msg_bytes == 0) {
    std::cerr << "asio_tcp_roundtrip: msg_bytes must be > 0\n";
    return 1;
  }

  net::io_context ioc;

  tcp::acceptor acceptor{ioc, tcp::endpoint{net::ip::make_address_v4("127.0.0.1"), 0}};
  auto const listen_ep = acceptor.local_endpoint();

  bench_state st{};
  st.ioc = &ioc;
  st.remaining_sessions.store(sessions);
  st.msgs_per_session = msgs;
  if (msg_bytes == 1) {
    st.msg = "\n";
  } else {
    st.msg.assign(msg_bytes - 1, 'x');
    st.msg.push_back('\n');
  }
  st.io_buffer_size = std::max<std::size_t>(4096, st.msg.size() * 2);

  auto const payload_bytes = st.msg.size();
  auto const total_roundtrips =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(msgs);
  auto const total_tx_bytes = total_roundtrips * payload_bytes;
  auto const total_rx_bytes = total_roundtrips * payload_bytes;

  co_spawn(ioc, accept_loop(acceptor, sessions, &st), detached);
  for (int i = 0; i < sessions; ++i) {
    co_spawn(ioc, client_session(ioc, listen_ep, &st), detached);
  }

  auto const start = std::chrono::steady_clock::now();
  ioc.run();
  auto const end = std::chrono::steady_clock::now();

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const rps = elapsed_s > 0.0 ? static_cast<double>(total_roundtrips) / elapsed_s : 0.0;
  auto const avg_us = total_roundtrips > 0 && elapsed_s > 0.0
                        ? (elapsed_s * 1'000'000.0) / static_cast<double>(total_roundtrips)
                        : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "asio_tcp_roundtrip"
            << " listen=" << listen_ep.address().to_string() << ":" << listen_ep.port()
            << " sessions=" << sessions << " msgs=" << msgs << " msg_bytes=" << payload_bytes
            << " roundtrips=" << total_roundtrips << " tx_bytes=" << total_tx_bytes
            << " rx_bytes=" << total_rx_bytes << " elapsed_s=" << elapsed_s << " rps=" << rps
            << " avg_us=" << avg_us << "\n";

  return 0;
}
