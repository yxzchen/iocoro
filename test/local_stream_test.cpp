#include <gtest/gtest.h>

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/impl.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/local/endpoint.hpp>
#include <iocoro/local/stream.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace {

using namespace std::chrono_literals;

struct unique_fd {
  int fd{-1};
  unique_fd() = default;
  explicit unique_fd(int f) : fd(f) {}
  unique_fd(unique_fd const&) = delete;
  auto operator=(unique_fd const&) -> unique_fd& = delete;
  unique_fd(unique_fd&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
  auto operator=(unique_fd&& o) noexcept -> unique_fd& {
    if (this != &o) {
      reset();
      fd = std::exchange(o.fd, -1);
    }
    return *this;
  }
  ~unique_fd() { reset(); }
  void reset(int f = -1) noexcept {
    if (fd >= 0) {
      (void)::close(fd);
    }
    fd = f;
  }
  explicit operator bool() const noexcept { return fd >= 0; }
};

struct unlink_guard {
  std::string path{};
  ~unlink_guard() {
    if (!path.empty()) {
      (void)::unlink(path.c_str());
    }
  }
};

static auto read_exact(int fd, void* data, std::size_t n) -> bool {
  auto* p = static_cast<std::byte*>(data);
  std::size_t off = 0;
  while (off < n) {
    auto r = ::read(fd, p + off, n - off);
    if (r > 0) {
      off += static_cast<std::size_t>(r);
      continue;
    }
    if (r == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

static auto write_all(int fd, void const* data, std::size_t n) -> bool {
  auto const* p = static_cast<std::byte const*>(data);
  std::size_t off = 0;
  while (off < n) {
    auto r = ::write(fd, p + off, n - off);
    if (r >= 0) {
      off += static_cast<std::size_t>(r);
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

static auto make_temp_unix_path() -> std::string {
  static std::atomic<unsigned> counter{0};
  auto const pid = static_cast<unsigned long>(::getpid());
  auto const n = counter.fetch_add(1, std::memory_order_relaxed);
  return std::string("/tmp/iocoro_local_stream_test_") + std::to_string(pid) + "_" +
         std::to_string(n) + ".sock";
}

static auto connect_to(iocoro::local::endpoint const& ep) -> unique_fd {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return unique_fd{};
  }

  sockaddr_storage ss{};
  auto len_r = ep.to_native(reinterpret_cast<sockaddr*>(&ss), sizeof(ss));
  if (!len_r) {
    (void)::close(fd);
    return unique_fd{};
  }

  if (::connect(fd, reinterpret_cast<sockaddr*>(&ss), *len_r) != 0) {
    (void)::close(fd);
    return unique_fd{};
  }

  return unique_fd{fd};
}

TEST(local_stream_acceptor_test, construction_and_executor) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  iocoro::local::stream::acceptor a{ctx};
  EXPECT_EQ(a.get_executor(), ex);
  EXPECT_FALSE(a.is_open());
  EXPECT_LT(a.native_handle(), 0);
}

TEST(local_stream_acceptor_test, async_accept_without_open_returns_not_open) {
  iocoro::io_context ctx;
  iocoro::local::stream::acceptor a{ctx};

  auto r = iocoro::sync_wait_for(ctx, 200ms, a.async_accept());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), iocoro::error::not_open);
}

TEST(local_stream_acceptor_test, open_bind_listen_accept_and_exchange_data) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto path = make_temp_unix_path();
  unlink_guard g{path};

  auto ep_r = iocoro::local::endpoint::from_path(path);
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto ec = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::local::stream::acceptor a{ex};
    if (auto e = a.listen(ep, 16)) {
      co_return e;
    }

    auto le = a.local_endpoint();
    if (!le) {
      co_return le.error();
    }
    if (le->family() != AF_UNIX) {
      co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    }

    unique_fd c = connect_to(ep);
    if (!c) {
      co_return std::error_code(errno, std::generic_category());
    }

    auto accepted = co_await a.async_accept();
    if (!accepted) {
      co_return accepted.error();
    }
    auto s = std::move(*accepted);
    if (!s.is_open() || !s.is_connected()) {
      co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    }

    // Client -> server
    {
      char const msg[] = "hi";
      if (!write_all(c.fd, msg, sizeof(msg) - 1)) {
        co_return std::error_code(errno, std::generic_category());
      }
      std::array<std::byte, 2> buf{};
      auto rr = co_await s.async_read_some(buf);
      if (!rr) {
        co_return rr.error();
      }
      if (*rr != 2U) {
        co_return iocoro::make_error_code(iocoro::error::invalid_argument);
      }
      if (static_cast<char>(buf[0]) != 'h' || static_cast<char>(buf[1]) != 'i') {
        co_return iocoro::make_error_code(iocoro::error::invalid_argument);
      }
    }

    // Server -> client
    {
      std::array<std::byte, 2> out{std::byte{'o'}, std::byte{'k'}};
      auto wr = co_await s.async_write_some(out);
      if (!wr) {
        co_return wr.error();
      }
      if (*wr != 2U) {
        co_return iocoro::make_error_code(iocoro::error::invalid_argument);
      }

      char got[2]{};
      if (!read_exact(c.fd, got, 2)) {
        co_return std::error_code(errno, std::generic_category());
      }
      if (got[0] != 'o' || got[1] != 'k') {
        co_return iocoro::make_error_code(iocoro::error::invalid_argument);
      }
    }

    co_return std::error_code{};
  }());

  ASSERT_FALSE(ec) << ec.message();
}

TEST(local_stream_acceptor_test, cancel_aborts_waiting_accept) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto path = make_temp_unix_path();
  unlink_guard g{path};
  auto ep_r = iocoro::local::endpoint::from_path(path);
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto got = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::local::stream::acceptor a{ex};
    if (auto ec = a.listen(ep, 16)) {
      co_return ec;
    }

    std::error_code out{};
    auto task = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto r = co_await a.async_accept();
        if (!r) {
          out = r.error();
        }
      },
      iocoro::use_awaitable);

    (void)co_await iocoro::co_sleep(ex, 10ms);
    a.cancel();

    try {
      co_await std::move(task);
    } catch (...) {
    }

    co_return out;
  }());

  EXPECT_EQ(got, iocoro::error::operation_aborted);
}

TEST(local_stream_acceptor_test, close_aborts_waiting_accept) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto path = make_temp_unix_path();
  unlink_guard g{path};
  auto ep_r = iocoro::local::endpoint::from_path(path);
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto got = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::local::stream::acceptor a{ex};
    if (auto ec = a.listen(ep, 16)) {
      co_return ec;
    }

    std::error_code out{};
    auto task = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto r = co_await a.async_accept();
        if (!r) {
          out = r.error();
        }
      },
      iocoro::use_awaitable);

    (void)co_await iocoro::co_sleep(ex, 10ms);
    a.close();

    try {
      co_await std::move(task);
    } catch (...) {
    }

    co_return out;
  }());

  EXPECT_EQ(got, iocoro::error::operation_aborted);
}

TEST(local_stream_socket_test, async_connect_and_exchange_data) {
  iocoro::io_context ctx;
  auto ex = ctx.get_executor();

  auto path = make_temp_unix_path();
  unlink_guard g{path};
  auto ep_r = iocoro::local::endpoint::from_path(path);
  ASSERT_TRUE(ep_r) << ep_r.error().message();
  auto ep = *ep_r;

  auto ec = iocoro::sync_wait_for(ctx, 1s, [&]() -> iocoro::awaitable<std::error_code> {
    iocoro::local::stream::acceptor a{ex};
    if (auto e = a.listen(ep, 16)) {
      co_return e;
    }

    std::error_code server_ec{};
    auto server_task = iocoro::co_spawn(
      ex,
      [&]() -> iocoro::awaitable<void> {
        auto accepted = co_await a.async_accept();
        if (!accepted) {
          server_ec = accepted.error();
          co_return;
        }
        auto s = std::move(*accepted);

        std::array<std::byte, 2> in{};
        auto rr = co_await s.async_read_some(in);
        if (!rr) {
          server_ec = rr.error();
          co_return;
        }

        std::array<std::byte, 2> out{std::byte{'o'}, std::byte{'k'}};
        auto wr = co_await s.async_write_some(out);
        if (!wr) {
          server_ec = wr.error();
        }
      },
      iocoro::use_awaitable);

    iocoro::local::stream::socket c{ex};
    if (auto e = co_await c.async_connect(ep)) {
      co_return e;
    }

    std::array<std::byte, 2> out{std::byte{'h'}, std::byte{'i'}};
    auto wr = co_await c.async_write_some(out);
    if (!wr) {
      co_return wr.error();
    }
    if (*wr != 2U) {
      co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    }

    std::array<std::byte, 2> in{};
    auto rr = co_await c.async_read_some(in);
    if (!rr) {
      co_return rr.error();
    }
    if (*rr != 2U) {
      co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    }
    if (static_cast<char>(in[0]) != 'o' || static_cast<char>(in[1]) != 'k') {
      co_return iocoro::make_error_code(iocoro::error::invalid_argument);
    }

    try {
      co_await std::move(server_task);
    } catch (...) {
    }
    if (server_ec) {
      co_return server_ec;
    }

    co_return std::error_code{};
  }());

  ASSERT_FALSE(ec) << ec.message();
}

}  // namespace


