#include <iocoro/iocoro.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using iocoro::ip::udp;

struct bench_state {
  iocoro::io_context* ctx = nullptr;
  std::atomic<int> remaining_events{0};
  int msgs_per_session = 0;
  std::size_t msg_bytes = 0;
};

inline void mark_done(bench_state* st) {
  if (st->remaining_events.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    st->ctx->stop();
  }
}

auto server_session(udp::socket socket, bench_state* st)
  -> iocoro::awaitable<void> {
  std::vector<std::byte> buffer(st->msg_bytes);
  udp::endpoint src{};
  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto r = co_await socket.async_receive_from(iocoro::net::buffer(buffer), src);
    if (!r || *r != buffer.size()) {
      st->ctx->stop();
      co_return;
    }
    auto w = co_await socket.async_send_to(iocoro::net::buffer(buffer), src);
    if (!w || *w != buffer.size()) {
      st->ctx->stop();
      co_return;
    }
  }
  mark_done(st);
}

auto client_session(udp::socket socket, udp::endpoint destination, bench_state* st)
  -> iocoro::awaitable<void> {
  std::vector<std::byte> payload(st->msg_bytes, std::byte{0x78});
  std::vector<std::byte> ack(st->msg_bytes);
  udp::endpoint src{};
  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto w = co_await socket.async_send_to(iocoro::net::buffer(payload), destination);
    if (!w || *w != payload.size()) {
      st->ctx->stop();
      co_return;
    }
    auto r = co_await socket.async_receive_from(iocoro::net::buffer(ack), src);
    if (!r || *r != ack.size()) {
      st->ctx->stop();
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
    std::cerr << "iocoro_udp_send_receive: sessions must be > 0\n";
    return 1;
  }
  if (msgs <= 0) {
    std::cerr << "iocoro_udp_send_receive: msgs must be > 0\n";
    return 1;
  }
  if (msg_bytes == 0) {
    std::cerr << "iocoro_udp_send_receive: msg_bytes must be > 0\n";
    return 1;
  }

  iocoro::io_context ctx;

  bench_state st{};
  st.ctx = &ctx;
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
    udp::socket server{ctx};
    auto bind_server = server.bind(udp::endpoint{iocoro::ip::address_v4::loopback(), 0});
    if (!bind_server) {
      std::cerr << "iocoro_udp_send_receive: server bind failed: " << bind_server.error().message()
                << "\n";
      return 1;
    }
    auto server_ep = server.local_endpoint();
    if (!server_ep) {
      std::cerr << "iocoro_udp_send_receive: server local_endpoint failed: "
                << server_ep.error().message() << "\n";
      return 1;
    }
    udp::socket client{ctx};
    auto bind_client = client.bind(udp::endpoint{iocoro::ip::address_v4::loopback(), 0});
    if (!bind_client) {
      std::cerr << "iocoro_udp_send_receive: client bind failed: " << bind_client.error().message()
                << "\n";
      return 1;
    }

    server_endpoints.push_back(*server_ep);
    server_sockets.push_back(std::move(server));
    client_sockets.push_back(std::move(client));
  }

  auto ex = ctx.get_executor();
  auto guard = iocoro::make_work_guard(ctx);

  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex, server_session(std::move(server_sockets[static_cast<std::size_t>(i)]), &st),
                     iocoro::detached);
  }
  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex,
                     client_session(std::move(client_sockets[static_cast<std::size_t>(i)]),
                                    server_endpoints[static_cast<std::size_t>(i)], &st),
                     iocoro::detached);
  }

  auto const total_messages =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(msgs);
  auto const total_bytes = total_messages * static_cast<std::uint64_t>(msg_bytes);

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const pps = elapsed_s > 0.0 ? static_cast<double>(total_messages) / elapsed_s : 0.0;
  auto const throughput_mib_s =
    elapsed_s > 0.0 ? (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / elapsed_s : 0.0;
  auto const avg_us = total_messages > 0 && elapsed_s > 0.0
                        ? (elapsed_s * 1'000'000.0) / static_cast<double>(total_messages)
                        : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "iocoro_udp_send_receive"
            << " sessions=" << sessions
            << " msgs=" << msgs
            << " msg_bytes=" << msg_bytes
            << " total_messages=" << total_messages
            << " total_bytes=" << total_bytes
            << " elapsed_s=" << elapsed_s
            << " pps=" << pps
            << " throughput_mib_s=" << throughput_mib_s
            << " avg_us=" << avg_us << "\n";

  return 0;
}
