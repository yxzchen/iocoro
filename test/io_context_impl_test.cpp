#include <gtest/gtest.h>

#include <iocoro/detail/io_context_impl.hpp>
#include <iocoro/detail/reactor_backend.hpp>
#include <iocoro/detail/reactor_types.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

struct noop_state {
  void on_complete() noexcept {}
  void on_abort(std::error_code) noexcept {}
};

inline auto thread_hash() -> std::size_t {
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

struct record_abort_thread_state {
  std::atomic<std::size_t>* abort_thread{};
  std::atomic<int>* abort_calls{};
  std::atomic<int>* complete_calls{};

  void on_complete() noexcept {
    if (complete_calls != nullptr) {
      complete_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }

  void on_abort(std::error_code) noexcept {
    if (abort_thread != nullptr) {
      abort_thread->store(thread_hash(), std::memory_order_relaxed);
    }
    if (abort_calls != nullptr) {
      abort_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }
};

struct expect_abort_ec_state {
  std::atomic<int>* abort_calls{};
  std::atomic<int>* complete_calls{};
  std::atomic<bool>* saw_expected{};
  std::error_code expected{};

  void on_complete() noexcept {
    if (complete_calls != nullptr) {
      complete_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }

  void on_abort(std::error_code ec) noexcept {
    if (abort_calls != nullptr) {
      abort_calls->fetch_add(1, std::memory_order_relaxed);
    }
    if (saw_expected != nullptr) {
      if (ec == expected) {
        saw_expected->store(true, std::memory_order_relaxed);
      }
    }
  }
};

struct post_on_abort_state {
  iocoro::detail::io_context_impl* impl{};
  std::atomic<int>* posted_calls{};
  std::atomic<int>* abort_calls{};

  void on_complete() noexcept {}

  void on_abort(std::error_code) noexcept {
    if (abort_calls != nullptr) {
      abort_calls->fetch_add(1, std::memory_order_relaxed);
    }
    if (impl != nullptr && posted_calls != nullptr) {
      impl->post([posted_calls = posted_calls]() {
        posted_calls->fetch_add(1, std::memory_order_relaxed);
      });
    }
  }
};

class backend_throw final : public iocoro::detail::backend_interface {
 public:
  std::atomic<int>* remove_calls{};
  std::atomic<int>* wakeup_calls{};
  int last_removed_fd{-1};

  explicit backend_throw(std::atomic<int>* remove_calls_, std::atomic<int>* wakeup_calls_) noexcept
      : remove_calls(remove_calls_), wakeup_calls(wakeup_calls_) {}

  void update_fd_interest(int /*fd*/, bool /*want_read*/, bool /*want_write*/) override {}

  void remove_fd_interest(int fd) noexcept override {
    last_removed_fd = fd;
    if (remove_calls != nullptr) {
      remove_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }

  auto wait(std::optional<std::chrono::milliseconds> /*timeout*/,
            std::vector<iocoro::detail::backend_event>& /*out*/) -> void override {
    throw std::runtime_error{"backend failure"};
  }

  void wakeup() noexcept override {
    if (wakeup_calls != nullptr) {
      wakeup_calls->fetch_add(1, std::memory_order_relaxed);
    }
  }
};

class backend_scripted final : public iocoro::detail::backend_interface {
 public:
  explicit backend_scripted(std::vector<iocoro::detail::backend_event> events)
      : events_(std::move(events)) {}

  void update_fd_interest(int /*fd*/, bool /*want_read*/, bool /*want_write*/) override {}
  void remove_fd_interest(int /*fd*/) noexcept override {}

  auto wait(std::optional<std::chrono::milliseconds> /*timeout*/,
            std::vector<iocoro::detail::backend_event>& out) -> void override {
    out = events_;
    events_.clear();
  }

  void wakeup() noexcept override {}

 private:
  std::vector<iocoro::detail::backend_event> events_{};
};

}  // namespace

TEST(io_context_impl_test, post_and_run_executes_operations) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<int> count{0};
  ctx->post([&] { ++count; });
  ctx->post([&] { ++count; });

  ctx->run();
  EXPECT_EQ(count.load(), 2);
}

TEST(io_context_impl_test, run_one_processes_single_task) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<int> count{0};
  ctx->post([&] {
    ++count;
    ctx->post([&] { ++count; });
  });

  ctx->run_one();
  EXPECT_EQ(count.load(), 1);

  ctx->run_one();
  EXPECT_EQ(count.load(), 2);
}

TEST(io_context_impl_test, run_for_without_work_returns_zero) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();
  auto n = ctx->run_for(std::chrono::milliseconds{1});
  EXPECT_EQ(n, 0U);
}

TEST(io_context_impl_test, schedule_timer_executes_callback) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<bool> fired{false};
  std::atomic<bool> aborted{false};

  struct timer_state {
    std::atomic<bool>* fired;
    std::atomic<bool>* aborted;

    void on_complete() noexcept { fired->store(true); }
    void on_abort(std::error_code) noexcept { aborted->store(true); }
  };

  auto op = iocoro::detail::make_reactor_op<timer_state>(&fired, &aborted);
  (void)ctx->add_timer(std::chrono::steady_clock::now(), std::move(op));

  ctx->run_one();
  EXPECT_TRUE(fired.load());
  EXPECT_FALSE(aborted.load());
}

TEST(io_context_impl_test, dispatch_runs_inline_on_context_thread) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::vector<int> order;
  ctx->post([&] {
    order.push_back(1);
    ctx->dispatch([&] { order.push_back(2); });
    order.push_back(3);
  });

  ctx->run();
  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

TEST(io_context_impl_test, run_for_processes_posted_work) {
  auto ctx = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<int> count{0};
  ctx->post([&] { ++count; });

  auto n = ctx->run_for(std::chrono::milliseconds{1});
  EXPECT_EQ(n, 1U);
  EXPECT_EQ(count.load(), 1);
}

TEST(io_context_impl_test, concurrent_run_is_rejected) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

  EXPECT_DEATH(
    {
      auto impl = std::make_shared<iocoro::detail::io_context_impl>();
      impl->add_work_guard();
      std::jthread runner{[&](std::stop_token) { (void)impl->run_for(1s); }};
      std::this_thread::sleep_for(5ms);
      (void)impl->run_one();
    },
    ".*");
}

TEST(io_context_impl_test, add_timer_wrong_thread_when_running_is_rejected) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

  EXPECT_DEATH(
    {
      auto impl = std::make_shared<iocoro::detail::io_context_impl>();
      impl->add_work_guard();
      std::jthread runner{[&](std::stop_token) { (void)impl->run_for(1s); }};
      std::this_thread::sleep_for(5ms);
      auto op = iocoro::detail::make_reactor_op<noop_state>();
      (void)impl->add_timer(std::chrono::steady_clock::now(), std::move(op));
    },
    ".*");
}

TEST(io_context_impl_test, cancel_timer_requires_shared_ownership) {
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";

  EXPECT_DEATH(
    {
      // Intentionally violate the contract: io_context_impl must be shared-owned.
      iocoro::detail::io_context_impl impl{};
      impl.cancel_timer(/*index=*/1U, /*generation=*/1U);
    },
    ".*");
}

TEST(io_context_impl_test, stress_cancel_timer_from_foreign_thread_does_not_invoke_abort_inline) {
  auto impl = std::make_shared<iocoro::detail::io_context_impl>();

  std::atomic<std::size_t> abort_tid{0};
  std::atomic<int> abort_calls{0};
  std::atomic<int> complete_calls{0};

  auto op = iocoro::detail::make_reactor_op<record_abort_thread_state>(
    record_abort_thread_state{&abort_tid, &abort_calls, &complete_calls});

  auto h =
    impl->add_timer(std::chrono::steady_clock::now() + std::chrono::seconds{10}, std::move(op));
  ASSERT_EQ(abort_calls.load(std::memory_order_relaxed), 0);
  ASSERT_EQ(complete_calls.load(std::memory_order_relaxed), 0);

  std::thread canceller([&] { impl->cancel_timer(h.timer_index, h.timer_generation); });
  canceller.join();

  EXPECT_EQ(abort_calls.load(std::memory_order_relaxed), 0);

  auto const run_tid = thread_hash();
  impl->run_one();

  ASSERT_EQ(abort_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(abort_tid.load(std::memory_order_relaxed), run_tid);
  EXPECT_EQ(complete_calls.load(std::memory_order_relaxed), 0);
}

TEST(io_context_impl_test, backend_throw_aborts_all_inflight_ops_and_stops_loop) {
  std::atomic<int> remove_calls{0};
  std::atomic<int> wakeup_calls{0};

  auto backend = std::make_unique<backend_throw>(&remove_calls, &wakeup_calls);
  auto* backend_ptr = backend.get();
  auto impl = std::make_shared<iocoro::detail::io_context_impl>(std::move(backend));

  int fds[2]{-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  ASSERT_GE(fds[0], 0);
  ASSERT_GE(fds[1], 0);

  std::atomic<int> posted_calls{0};
  std::atomic<int> abort_calls{0};
  auto fd_op = iocoro::detail::make_reactor_op<post_on_abort_state>(
    post_on_abort_state{impl.get(), &posted_calls, &abort_calls});
  auto fd_h = impl->register_fd_read(fds[0], std::move(fd_op));
  ASSERT_TRUE(static_cast<bool>(fd_h));

  std::atomic<bool> timer_aborted{false};
  std::atomic<int> timer_abort_calls{0};
  std::atomic<int> timer_complete_calls{0};
  auto expected_internal = iocoro::make_error_code(iocoro::error::internal_error);
  auto timer_op = iocoro::detail::make_reactor_op<expect_abort_ec_state>(
    expect_abort_ec_state{&timer_abort_calls, &timer_complete_calls, &timer_aborted,
                          expected_internal});
  auto timer_h =
    impl->add_timer(std::chrono::steady_clock::now() + std::chrono::hours{1}, std::move(timer_op));
  ASSERT_TRUE(static_cast<bool>(timer_h));

  auto n = impl->run_one();
  EXPECT_EQ(n, 0U);
  EXPECT_TRUE(impl->stopped());

  EXPECT_EQ(timer_complete_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(timer_abort_calls.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(timer_aborted.load(std::memory_order_relaxed));

  EXPECT_EQ(abort_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(posted_calls.load(std::memory_order_relaxed), 1);

  EXPECT_EQ(remove_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(backend_ptr->last_removed_fd, fds[0]);

  (void)::close(fds[0]);
  (void)::close(fds[1]);
}

TEST(io_context_impl_test, backend_error_event_is_routed_to_matching_fd_ops) {
  int fds[2]{-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  ASSERT_GE(fds[0], 0);
  ASSERT_GE(fds[1], 0);

  auto injected_ec = std::make_error_code(std::errc::io_error);
  std::vector<iocoro::detail::backend_event> evs;
  iocoro::detail::backend_event ev{};
  ev.fd = fds[0];
  ev.can_read = true;
  ev.can_write = false;
  ev.is_error = true;
  ev.ec = injected_ec;
  evs.push_back(ev);

  auto backend = std::make_unique<backend_scripted>(std::move(evs));
  auto impl = std::make_shared<iocoro::detail::io_context_impl>(std::move(backend));

  std::atomic<int> abort_calls{0};
  std::atomic<int> complete_calls{0};
  std::atomic<bool> saw_ec{false};

  auto op = iocoro::detail::make_reactor_op<expect_abort_ec_state>(
    expect_abort_ec_state{&abort_calls, &complete_calls, &saw_ec, injected_ec});
  auto h = impl->register_fd_read(fds[0], std::move(op));
  ASSERT_TRUE(static_cast<bool>(h));

  auto n = impl->run_one();
  EXPECT_EQ(n, 1U);

  EXPECT_EQ(complete_calls.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(abort_calls.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(saw_ec.load(std::memory_order_relaxed));

  (void)::close(fds[0]);
  (void)::close(fds[1]);
}

TEST(io_context_impl_test, cancel_fd_from_foreign_thread_does_not_invoke_abort_inline) {
  auto impl = std::make_shared<iocoro::detail::io_context_impl>();

  int fds[2]{-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  ASSERT_GE(fds[0], 0);
  ASSERT_GE(fds[1], 0);

  std::atomic<std::size_t> abort_tid{0};
  std::atomic<int> abort_calls{0};
  std::atomic<int> complete_calls{0};

  auto op = iocoro::detail::make_reactor_op<record_abort_thread_state>(
    record_abort_thread_state{&abort_tid, &abort_calls, &complete_calls});
  auto h = impl->register_fd_read(fds[0], std::move(op));
  ASSERT_TRUE(static_cast<bool>(h));

  std::thread canceller([&] {
    impl->cancel_fd_event(h.fd, h.fd_kind, h.token);
  });
  canceller.join();

  EXPECT_EQ(abort_calls.load(std::memory_order_relaxed), 0);

  auto const run_tid = thread_hash();
  impl->run_one();

  EXPECT_EQ(complete_calls.load(std::memory_order_relaxed), 0);
  ASSERT_EQ(abort_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(abort_tid.load(std::memory_order_relaxed), run_tid);

  (void)::close(fds[0]);
  (void)::close(fds[1]);
}
