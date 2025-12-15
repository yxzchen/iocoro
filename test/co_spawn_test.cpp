#include <xz/io.hpp>
#include <xz/io/src.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

using namespace std::chrono_literals;
using namespace xz::io;

TEST(CoSpawnTest, BasicDetached) {
  io_context ctx;
  std::atomic<int> counter{0};

  auto simple_task = [&]() -> awaitable<void> {
    counter++;
    co_return;
  };

  co_spawn(ctx, simple_task, use_detached);

  ctx.run();
  EXPECT_EQ(counter.load(), 1);
}

TEST(CoSpawnTest, MultipleSpawns) {
  io_context ctx;
  std::atomic<int> counter{0};

  auto increment_task = [&]() -> awaitable<void> {
    counter++;
    co_return;
  };

  co_spawn(ctx, increment_task, use_detached);
  co_spawn(ctx, increment_task, use_detached);
  co_spawn(ctx, increment_task, use_detached);

  ctx.run();
  EXPECT_EQ(counter.load(), 3);
}

TEST(CoSpawnTest, WithCallable) {
  io_context ctx;
  std::atomic<bool> executed{false};

  // Pass a callable directly (lambda that returns a task)
  co_spawn(ctx, [&]() -> awaitable<void> {
    executed.store(true);
    co_return;
  }, use_detached);

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(CoSpawnTest, WithTimer) {
  io_context ctx;
  std::atomic<bool> timer_fired{false};

  auto timer_task = [&]() -> awaitable<void> {
    // Create a simple async operation using a timer
    ctx.schedule_timer(50ms, [&]() {
      timer_fired.store(true);
      ctx.stop();
    });
    co_return;
  };

  co_spawn(ctx, timer_task, use_detached);

  auto start = std::chrono::steady_clock::now();
  ctx.run();
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(timer_fired.load());
  EXPECT_GE(elapsed, 49ms);
  EXPECT_LT(elapsed, 150ms);
}

TEST(CoSpawnTest, ExceptionHandling) {
  io_context ctx;
  std::atomic<int> counter{0};

  auto throwing_task = [&]() -> awaitable<void> {
    counter++;
    throw std::runtime_error("test exception");
    co_return;
  };

  auto normal_task = [&]() -> awaitable<void> {
    counter++;
    co_return;
  };

  // Spawn a task that throws
  co_spawn(ctx, throwing_task, use_detached);
  // Spawn a normal task to ensure execution continues
  co_spawn(ctx, normal_task, use_detached);

  // Should not throw - exceptions are swallowed in detached mode
  EXPECT_NO_THROW(ctx.run());

  // Both tasks should have started (counter should be 2)
  EXPECT_EQ(counter.load(), 2);
}

TEST(CoSpawnTest, CompletionHandlerSuccess_DirectAwaitable) {
  io_context ctx;

  std::atomic<int> called{0};
  std::exception_ptr captured;

  auto task = [&]() -> awaitable<void> { co_return; };

  co_spawn(ctx, task, [&](std::exception_ptr eptr) {
    captured = eptr;
    called.fetch_add(1);
    ctx.stop();
  });

  EXPECT_NO_THROW(ctx.run());
  EXPECT_EQ(called.load(), 1);
  EXPECT_EQ(captured, nullptr);
}

TEST(CoSpawnTest, CompletionHandlerException_CallableFactory) {
  io_context ctx;

  std::atomic<int> called{0};
  std::string what;

  co_spawn(ctx, [&]() -> awaitable<void> {
    throw std::runtime_error("boom");
    co_return;
  }, [&](std::exception_ptr eptr) {
    called.fetch_add(1);
    if (eptr) {
      try {
        std::rethrow_exception(eptr);
      } catch (std::exception const& e) {
        what = e.what();
      }
    }
    ctx.stop();
  });

  EXPECT_NO_THROW(ctx.run());
  EXPECT_EQ(called.load(), 1);
  EXPECT_EQ(what, "boom");
}

TEST(CoSpawnTest, WithTcpSocket) {
  io_context ctx;
  tcp_socket socket(ctx);
  std::atomic<bool> connected{false};

  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  co_spawn(ctx, [&]() -> awaitable<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);
      connected.store(true);
      socket.close();
    } catch (std::system_error const&) {
      // Connection failed
    }
  }, use_detached);

  ctx.run();
  EXPECT_TRUE(connected.load());
}

TEST(CoSpawnTest, ChainedOperations) {
  io_context ctx;
  tcp_socket socket(ctx);
  std::atomic<bool> success{false};

  auto endpoint = ip::tcp_endpoint{ip::address_v4{{127, 0, 0, 1}}, 6379};

  co_spawn(ctx, [&]() -> awaitable<void> {
    try {
      co_await socket.async_connect(endpoint, 1000ms);

      // Send PING
      std::string cmd = "*1\r\n$4\r\nPING\r\n";
      co_await socket.async_write_some(
          std::span<char const>{cmd.data(), cmd.size()}, 1000ms);

      // Read response
      char buffer[256]{};
      auto bytes_read = co_await socket.async_read_some(
          std::span<char>{buffer, sizeof(buffer)}, 1000ms);

      if (bytes_read > 0) {
        std::string response(buffer, bytes_read);
        if (response.find("PONG") != std::string::npos) {
          success.store(true);
        }
      }

      socket.close();
    } catch (std::system_error const&) {
      // Operation failed
    }
  }, use_detached);

  ctx.run();
  EXPECT_TRUE(success.load());
}

TEST(CoSpawnTest, TaskWithReturnValue) {
  io_context ctx;
  std::atomic<int> result{0};

  auto value_task = [&]() -> awaitable<int> {
    co_return 42;
  };

  co_spawn(ctx, [&, value_task]() -> awaitable<void> {
    auto value = co_await value_task();
    result.store(value);
  }, use_detached);

  ctx.run();
  EXPECT_EQ(result.load(), 42);
}

TEST(CoSpawnTest, MultipleNestedTasks) {
  io_context ctx;
  std::atomic<int> sum{0};

  auto add_value = [&](int value) -> awaitable<void> {
    sum.fetch_add(value);
    co_return;
  };

  co_spawn(ctx, [&, add_value]() -> awaitable<void> {
    co_await add_value(10);
    co_await add_value(20);
    co_await add_value(30);
  }, use_detached);

  ctx.run();
  EXPECT_EQ(sum.load(), 60);
}

TEST(CoSpawnTest, LambdaOutOfScopeBeforeExecution) {
  io_context ctx;
  std::atomic<int> result{0};
  std::atomic<bool> executed{false};

  // Create lambda and its captures in a nested scope
  {
    int value = 42;
    std::string message = "test_message";

    // Spawn a lambda that captures by value
    co_spawn(ctx, [value, message, &result, &executed]() -> awaitable<void> {
      // At this point, the original 'value' and 'message' variables are out of scope
      // But the lambda should have captured copies
      result.store(value);
      executed.store(message == "test_message");
      co_return;
    }, use_detached);

    // Lambda and captured variables go out of scope here
  }

  // Now run the event loop - the spawned coroutine should still work
  ctx.run();

  EXPECT_EQ(result.load(), 42);
  EXPECT_TRUE(executed.load());
}

TEST(CoSpawnTest, LambdaWithComplexCaptureOutOfScope) {
  io_context ctx;
  std::atomic<int> sum_result{0};
  std::atomic<bool> prefix_valid{false};
  std::atomic<bool> executed{false};

  // Create a helper coroutine function that takes parameters instead of captures
  // This is the correct pattern - pass data as parameters, not as lambda captures
  auto process_data = [](std::vector<int> nums, std::string pref,
                         std::atomic<int>& sum_ref,
                         std::atomic<bool>& prefix_ref,
                         std::atomic<bool>& exec_ref) -> awaitable<void> {
    exec_ref.store(true);
    int sum = 0;
    for (int n : nums) {
      sum += n;
    }
    sum_ref.store(sum);
    prefix_ref.store(pref == "Number: ");
    co_return;
  };

  {
    // Create complex objects that will go out of scope
    std::vector<int> numbers = {1, 2, 3, 4, 5};
    std::string prefix = "Number: ";

    // Spawn the coroutine - parameters are copied into the coroutine frame
    co_spawn(ctx,
             [numbers, prefix, &sum_result, &prefix_valid, &executed, process_data]() mutable -> awaitable<void> {
               co_await process_data(numbers, prefix, sum_result, prefix_valid, executed);
             },
             use_detached);

    // All local variables destroyed here
  }

  ctx.run();
  EXPECT_TRUE(executed.load()) << "Coroutine was not executed";
  EXPECT_EQ(sum_result.load(), 15) << "Sum calculation failed";
  EXPECT_TRUE(prefix_valid.load()) << "Prefix validation failed";
}

TEST(CoSpawnTest, CallableFactoryOutOfScope) {
  io_context ctx;
  std::atomic<int> result{0};

  // Helper that returns a lambda (factory pattern)
  auto make_task = [](int value) {
    return [value]() -> awaitable<void> {
      // Simulate some work
      co_return;
    };
  };

  {
    int temp_value = 100;
    auto task = make_task(temp_value);

    co_spawn(ctx, [task, &result]() -> awaitable<void> {
      // task lambda is captured by value, temp_value is out of scope
      co_await task();
      result.store(999);
      co_return;
    }, use_detached);

    // temp_value and task go out of scope
  }

  ctx.run();
  EXPECT_EQ(result.load(), 999);
}

TEST(CoSpawnTest, MoveParametersIntoCoroutine) {
  io_context ctx;
  std::atomic<int> sum_result{0};
  std::atomic<bool> prefix_valid{false};
  std::atomic<bool> executed{false};

  // Coroutine function that takes parameters by value (enabling move)
  auto process = [](std::vector<int> nums, std::string pref,
                    std::atomic<int>& sum_ref,
                    std::atomic<bool>& prefix_ref,
                    std::atomic<bool>& exec_ref) -> awaitable<void> {
    exec_ref.store(true);

    int sum = 0;
    for (int n : nums) {
      sum += n;
    }
    sum_ref.store(sum);
    prefix_ref.store(pref == "Moved: ");
    co_return;
  };

  {
    // Create complex objects that will be moved
    std::vector<int> numbers = {1, 2, 3, 4, 5};
    std::string prefix = "Moved: ";

    // Move parameters into coroutine - no copy!
    co_spawn(ctx,
             [numbers = std::move(numbers), prefix = std::move(prefix),
              &sum_result, &prefix_valid, &executed, process]() mutable -> awaitable<void> {
               co_await process(std::move(numbers), std::move(prefix),
                                sum_result, prefix_valid, executed);
             },
             use_detached);

    // Original numbers and prefix are now moved-from (empty)
    EXPECT_TRUE(numbers.empty()) << "Vector should be moved-from";
    EXPECT_TRUE(prefix.empty()) << "String should be moved-from";

    // All variables destroyed here
  }

  ctx.run();
  EXPECT_TRUE(executed.load()) << "Coroutine was not executed";
  EXPECT_EQ(sum_result.load(), 15) << "Sum calculation failed";
  EXPECT_TRUE(prefix_valid.load()) << "Prefix validation failed";
}

TEST(CoSpawnTest, MoveOnlyTypeAsParameter) {
  io_context ctx;
  std::atomic<bool> success{false};
  std::atomic<bool> executed{false};

  // Coroutine that accepts move-only type as parameter
  auto process = [](std::unique_ptr<int> data,
                    std::atomic<bool>& success_ref,
                    std::atomic<bool>& exec_ref) -> awaitable<void> {
    exec_ref.store(true);
    success_ref.store(*data == 42);
    co_return;
  };

  {
    // Create move-only type
    auto unique_data = std::make_unique<int>(42);

    // Move unique_ptr into coroutine as parameter
    co_spawn(ctx,
             [data = std::move(unique_data), &success, &executed, process]() mutable -> awaitable<void> {
               co_await process(std::move(data), success, executed);
             },
             use_detached);

    // unique_data is now nullptr (moved-from)
    EXPECT_EQ(unique_data, nullptr) << "unique_ptr should be moved-from";
  }

  ctx.run();
  EXPECT_TRUE(executed.load()) << "Coroutine was not executed";
  EXPECT_TRUE(success.load()) << "Data validation failed";
}

TEST(CoSpawnTest, DirectMoveParametersIntoCoroutine) {
  io_context ctx;
  std::atomic<int> sum_result{0};
  std::atomic<bool> executed{false};

  // Coroutine that takes parameters by value (will be moved)
  auto process = [](std::vector<int> nums, std::unique_ptr<std::string> msg,
                    std::atomic<int>& sum_ref, std::atomic<bool>& exec_ref) -> awaitable<void> {
    exec_ref.store(true);

    int sum = 0;
    for (int n : nums) {
      sum += n;
    }
    sum_ref.store(sum);
    co_return;
  };

  {
    std::vector<int> numbers = {10, 20, 30};
    auto message = std::make_unique<std::string>("test");

    // Move parameters directly into coroutine
    co_spawn(ctx,
             [numbers = std::move(numbers), msg = std::move(message),
              &sum_result, &executed, process]() mutable -> awaitable<void> {
               co_await process(std::move(numbers), std::move(msg), sum_result, executed);
             },
             use_detached);

    // Verify moved-from state
    EXPECT_TRUE(numbers.empty()) << "Vector should be moved";
    EXPECT_EQ(message, nullptr) << "unique_ptr should be moved";
  }

  ctx.run();
  EXPECT_TRUE(executed.load()) << "Coroutine was not executed";
  EXPECT_EQ(sum_result.load(), 60) << "Sum should be 60";
}
