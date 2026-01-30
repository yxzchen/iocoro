#include <iocoro/awaitable.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/completion_token.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io/write.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/ip.hpp>
#include <iocoro/this_coro.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {

using iocoro::ip::tcp;

auto echo(tcp::socket socket) -> iocoro::awaitable<void> {
  try {
    std::array<std::byte, 1024> data{};
    for (;;) {
      auto r = co_await socket.async_read_some(std::span<std::byte>(data));
      if (!r) {
        co_return;
      }
      auto const n = *r;
      if (n == 0) {
        co_return;
      }
      auto w = co_await iocoro::io::async_write(
        socket, std::span<std::byte const>(data.data(), n));
      if (!w) {
        co_return;
      }
    }
  } catch (std::exception const&) {
  }
}

auto listener(iocoro::io_context& ctx) -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;
  tcp::acceptor acceptor{ctx};
  auto ep = tcp::endpoint{iocoro::ip::address_v4::loopback(), 55555};
  if (auto ec = acceptor.listen(ep)) {
    throw std::runtime_error("listen failed");
  }
  std::cout << "iocoro_tcp_server listening on " << ep.to_string() << "\n";
  for (;;) {
    auto accepted = co_await acceptor.async_accept();
    if (!accepted) {
      continue;
    }
    iocoro::co_spawn(ex, echo(std::move(*accepted)), iocoro::detached);
  }
}

}  // namespace

int main() {
  try {
    iocoro::io_context ctx;
    auto ex = ctx.get_executor();
    iocoro::co_spawn(ex, listener(ctx), iocoro::detached);
    ctx.run();
  } catch (std::exception const& ex) {
    std::cerr << "server failed: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
