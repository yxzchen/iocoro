#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace net = boost::asio;
using net::awaitable;
using net::co_spawn;
using net::detached;
using net::use_awaitable;
using net::ip::udp;

namespace {

struct bench_state {
  net::io_context* ioc = nullptr;
  std::atomic<int> remaining_events{0};
  std::atomic<bool> failed{false};
  int msgs_per_session = 0;
  std::size_t msg_bytes = 0;
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

auto server_session(udp::socket socket, bench_state* st) -> awaitable<void> {
  std::vector<std::byte> buffer(st->msg_bytes);
  udp::endpoint src{};
  for (int i = 0; i < st->msgs_per_session; ++i) {
    boost::system::error_code ec;
    auto n = co_await socket.async_receive_from(net::buffer(buffer), src,
                                                net::redirect_error(use_awaitable, ec));
    if (ec || n != buffer.size()) {
      if (ec) {
        fail_and_stop(st, "asio_udp_send_receive: server receive failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_udp_send_receive: server receive size mismatch");
      }
      co_return;
    }
    n = co_await socket.async_send_to(net::buffer(buffer), src,
                                      net::redirect_error(use_awaitable, ec));
    if (ec || n != buffer.size()) {
      if (ec) {
        fail_and_stop(st, "asio_udp_send_receive: server send failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_udp_send_receive: server send size mismatch");
      }
      co_return;
    }
  }
  mark_done(st);
}

auto client_session(udp::socket socket, udp::endpoint destination,
                    bench_state* st) -> awaitable<void> {
  std::vector<std::byte> payload(st->msg_bytes, std::byte{0x78});
  std::vector<std::byte> ack(st->msg_bytes);
  udp::endpoint src{};
  for (int i = 0; i < st->msgs_per_session; ++i) {
    boost::system::error_code ec;
    auto n = co_await socket.async_send_to(net::buffer(payload), destination,
                                           net::redirect_error(use_awaitable, ec));
    if (ec || n != payload.size()) {
      if (ec) {
        fail_and_stop(st, "asio_udp_send_receive: client send failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_udp_send_receive: client send size mismatch");
      }
      co_return;
    }
    n = co_await socket.async_receive_from(net::buffer(ack), src,
                                           net::redirect_error(use_awaitable, ec));
    if (ec || n != ack.size()) {
      if (ec) {
        fail_and_stop(st, "asio_udp_send_receive: client receive failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_udp_send_receive: client receive size mismatch");
      }
      co_return;
    }
  }
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
    std::cerr << "asio_udp_send_receive: sessions must be > 0\n";
    return 1;
  }
  if (msgs <= 0) {
    std::cerr << "asio_udp_send_receive: msgs must be > 0\n";
    return 1;
  }
  if (msg_bytes == 0) {
    std::cerr << "asio_udp_send_receive: msg_bytes must be > 0\n";
    return 1;
  }

  net::io_context ioc;

  bench_state st{};
  st.ioc = &ioc;
  st.msgs_per_session = msgs;
  st.msg_bytes = msg_bytes;
  st.remaining_events.store(sessions * 2, std::memory_order_release);

  std::vector<udp::endpoint> server_endpoints;
  server_endpoints.reserve(static_cast<std::size_t>(sessions));
  std::vector<udp::socket> server_sockets;
  server_sockets.reserve(static_cast<std::size_t>(sessions));
  std::vector<udp::socket> client_sockets;
  client_sockets.reserve(static_cast<std::size_t>(sessions));

  for (int i = 0; i < sessions; ++i) {
    udp::socket server{ioc};
    boost::system::error_code ec;
    server.open(udp::v4(), ec);
    if (ec) {
      std::cerr << "asio_udp_send_receive: server open failed: " << ec.message() << "\n";
      return 1;
    }
    server.bind(udp::endpoint{net::ip::make_address_v4("127.0.0.1"), 0}, ec);
    if (ec) {
      std::cerr << "asio_udp_send_receive: server bind failed: " << ec.message() << "\n";
      return 1;
    }
    auto server_ep = server.local_endpoint(ec);
    if (ec) {
      std::cerr << "asio_udp_send_receive: server local_endpoint failed: " << ec.message() << "\n";
      return 1;
    }

    udp::socket client{ioc};
    client.open(udp::v4(), ec);
    if (ec) {
      std::cerr << "asio_udp_send_receive: client open failed: " << ec.message() << "\n";
      return 1;
    }
    client.bind(udp::endpoint{net::ip::make_address_v4("127.0.0.1"), 0}, ec);
    if (ec) {
      std::cerr << "asio_udp_send_receive: client bind failed: " << ec.message() << "\n";
      return 1;
    }

    server_endpoints.push_back(server_ep);
    server_sockets.push_back(std::move(server));
    client_sockets.push_back(std::move(client));
  }

  auto ex = ioc.get_executor();
  for (int i = 0; i < sessions; ++i) {
    co_spawn(ex, server_session(std::move(server_sockets[static_cast<std::size_t>(i)]), &st),
             detached);
  }
  for (int i = 0; i < sessions; ++i) {
    co_spawn(ex,
             client_session(std::move(client_sockets[static_cast<std::size_t>(i)]),
                            server_endpoints[static_cast<std::size_t>(i)], &st),
             detached);
  }

  auto const total_messages =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(msgs);
  auto const total_bytes = total_messages * static_cast<std::uint64_t>(msg_bytes);

  auto const start = std::chrono::steady_clock::now();
  ioc.run();
  auto const end = std::chrono::steady_clock::now();

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }
  if (st.remaining_events.load(std::memory_order_acquire) != 0) {
    std::cerr << "asio_udp_send_receive: incomplete run (remaining_events="
              << st.remaining_events.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const pps = elapsed_s > 0.0 ? static_cast<double>(total_messages) / elapsed_s : 0.0;
  auto const throughput_mib_s =
    elapsed_s > 0.0 ? (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / elapsed_s : 0.0;
  auto const avg_us = total_messages > 0 && elapsed_s > 0.0
                        ? (elapsed_s * 1'000'000.0) / static_cast<double>(total_messages)
                        : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "asio_udp_send_receive"
            << " sessions=" << sessions << " msgs=" << msgs << " msg_bytes=" << msg_bytes
            << " total_messages=" << total_messages << " total_bytes=" << total_bytes
            << " elapsed_s=" << elapsed_s << " pps=" << pps
            << " throughput_mib_s=" << throughput_mib_s << " avg_us=" << avg_us << "\n";

  return 0;
}
