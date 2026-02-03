// tcp_echo.cpp
//
// Purpose:
//   Minimal end-to-end TCP example in a single process:
//   - Start an acceptor on 127.0.0.1:0 (ephemeral port)
//   - Query acceptor.local_endpoint()
//   - Connect a client socket, write a line, read the echoed line
//
// Preconditions:
//   - Loopback networking is available on the host.
//
// Notes (development stage):
//   - This example demonstrates usage only. Error and cancellation semantics may change.

#include <iocoro/iocoro.hpp>
#include <iocoro/ip.hpp>

#include <cstddef>
#include <iostream>
#include <span>
#include <string>

namespace {

using iocoro::ip::tcp;

auto server_once(tcp::acceptor& acceptor) -> iocoro::awaitable<void> {
  auto accepted = co_await acceptor.async_accept();
  if (!accepted) {
    co_return;
  }

  auto socket = std::move(*accepted);
  std::string buffer;
  buffer.resize(1024);
  auto span = std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()), buffer.size());

  auto r = co_await iocoro::io::async_read_until(socket, span, '\n', 0);
  if (!r) {
    co_return;
  }

  auto const n = *r;
  auto w = co_await iocoro::io::async_write(
    socket, std::span<std::byte const>(span.data(), n));
  if (!w) {
    co_return;
  }
}

auto client_once(iocoro::io_context& ctx, tcp::endpoint ep) -> iocoro::awaitable<void> {
  tcp::socket socket{ctx};
  auto cr = co_await socket.async_connect(ep);
  if (!cr) {
    co_return;
  }

  std::string msg = "ping\n";
  auto w = co_await iocoro::io::async_write(
    socket,
    std::span<std::byte const>(reinterpret_cast<std::byte const*>(msg.data()), msg.size()));
  if (!w) {
    co_return;
  }

  std::string buffer;
  buffer.resize(1024);
  auto span = std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()), buffer.size());
  auto r = co_await iocoro::io::async_read_until(socket, span, '\n', 0);
  if (!r) {
    co_return;
  }

  auto const n = *r;
  std::cout << "tcp_echo: received: "
            << std::string_view(buffer.data(), n);

  ctx.stop();
}

}  // namespace

int main() {
  iocoro::io_context ctx;

  tcp::acceptor acceptor{ctx};
  auto listen_ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), 0};
  auto lr = acceptor.listen(listen_ep);
  if (!lr) {
    std::cerr << "tcp_echo: listen failed\n";
    return 1;
  }

  auto ep_r = acceptor.local_endpoint();
  if (!ep_r) {
    std::cerr << "tcp_echo: local_endpoint failed\n";
    return 1;
  }

  auto ex = ctx.get_executor();

  auto guard = iocoro::make_work_guard(ctx);
  iocoro::co_spawn(ex, server_once(acceptor), iocoro::detached);
  iocoro::co_spawn(ex, client_once(ctx, *ep_r), iocoro::detached);

  ctx.run();
  return 0;
}

