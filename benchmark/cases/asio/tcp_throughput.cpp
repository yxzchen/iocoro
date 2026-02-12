#include <boost/asio.hpp>

#include <algorithm>
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
using net::ip::tcp;
using net::use_awaitable;

namespace {

struct bench_state {
  net::io_context* ioc = nullptr;
  std::atomic<int> remaining_events{0};
  std::atomic<bool> failed{false};
  std::uint64_t bytes_per_session = 0;
  std::size_t chunk_bytes = 0;
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

auto server_session(tcp::socket socket, bench_state* st) -> awaitable<void> {
  std::vector<std::byte> read_buf(st->chunk_bytes);
  auto remaining = st->bytes_per_session;

  while (remaining > 0) {
    auto const to_read =
      static_cast<std::size_t>(std::min<std::uint64_t>(remaining, read_buf.size()));
    boost::system::error_code ec;
    auto n =
      co_await socket.async_read_some(net::buffer(read_buf.data(), to_read), net::redirect_error(use_awaitable, ec));
    if (ec || n == 0) {
      if (ec) {
        fail_and_stop(st, "asio_tcp_throughput: server read failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_tcp_throughput: server read returned 0");
      }
      co_return;
    }
    remaining -= static_cast<std::uint64_t>(n);
  }

  mark_done(st);
}

auto accept_loop(tcp::acceptor& acceptor, int sessions, bench_state* st) -> awaitable<void> {
  auto ex = co_await net::this_coro::executor;
  for (int i = 0; i < sessions; ++i) {
    boost::system::error_code ec;
    auto socket = co_await acceptor.async_accept(net::redirect_error(use_awaitable, ec));
    if (ec) {
      fail_and_stop(st, "asio_tcp_throughput: accept failed: " + ec.message());
      co_return;
    }
    co_spawn(ex, server_session(std::move(socket), st), detached);
  }
}

auto client_session(net::io_context& ioc, tcp::endpoint ep, bench_state* st) -> awaitable<void> {
  auto ex = co_await net::this_coro::executor;
  tcp::socket socket{ex};

  boost::system::error_code ec;
  co_await socket.async_connect(ep, net::redirect_error(use_awaitable, ec));
  if (ec) {
    fail_and_stop(st, "asio_tcp_throughput: connect failed: " + ec.message());
    co_return;
  }

  std::vector<std::byte> payload(st->chunk_bytes, std::byte{0x78});
  auto remaining = st->bytes_per_session;

  while (remaining > 0) {
    auto const to_send =
      static_cast<std::size_t>(std::min<std::uint64_t>(remaining, payload.size()));
    auto n =
      co_await net::async_write(socket, net::buffer(payload.data(), to_send), net::redirect_error(use_awaitable, ec));
    if (ec || n == 0) {
      if (ec) {
        fail_and_stop(st, "asio_tcp_throughput: client write failed: " + ec.message());
      } else {
        fail_and_stop(st, "asio_tcp_throughput: client write returned 0");
      }
      co_return;
    }
    remaining -= static_cast<std::uint64_t>(n);
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
    std::cerr << "asio_tcp_throughput: sessions must be > 0\n";
    return 1;
  }
  if (bytes_per_session == 0) {
    std::cerr << "asio_tcp_throughput: bytes_per_session must be > 0\n";
    return 1;
  }
  if (chunk_bytes == 0) {
    std::cerr << "asio_tcp_throughput: chunk_bytes must be > 0\n";
    return 1;
  }

  net::io_context ioc;

  tcp::acceptor acceptor{ioc, tcp::endpoint{net::ip::make_address_v4("127.0.0.1"), 0}};
  auto const listen_ep = acceptor.local_endpoint();

  bench_state st{};
  st.ioc = &ioc;
  st.bytes_per_session = bytes_per_session;
  st.chunk_bytes = chunk_bytes;
  st.remaining_events.store(sessions * 2, std::memory_order_release);

  co_spawn(ioc, accept_loop(acceptor, sessions, &st), detached);
  for (int i = 0; i < sessions; ++i) {
    co_spawn(ioc, client_session(ioc, listen_ep, &st), detached);
  }

  auto const total_bytes =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(bytes_per_session);

  auto const start = std::chrono::steady_clock::now();
  ioc.run();
  auto const end = std::chrono::steady_clock::now();

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }
  if (st.remaining_events.load(std::memory_order_acquire) != 0) {
    std::cerr << "asio_tcp_throughput: incomplete run (remaining_events="
              << st.remaining_events.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const throughput_mib_s =
    elapsed_s > 0.0 ? (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / elapsed_s : 0.0;
  auto const avg_session_ms =
    sessions > 0 && elapsed_s > 0.0 ? (elapsed_s * 1000.0) / static_cast<double>(sessions) : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "asio_tcp_throughput"
            << " listen=" << listen_ep.address().to_string() << ":" << listen_ep.port()
            << " sessions=" << sessions
            << " bytes_per_session=" << bytes_per_session
            << " chunk_bytes=" << chunk_bytes
            << " total_bytes=" << total_bytes
            << " elapsed_s=" << elapsed_s
            << " throughput_mib_s=" << throughput_mib_s
            << " avg_session_ms=" << avg_session_ms << "\n";

  return 0;
}
