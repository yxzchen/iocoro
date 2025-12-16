#include <xz/io.hpp>
#include <xz/io/src.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <variant>

using namespace std::chrono_literals;
using namespace xz::io;

static void fail_and_stop_on_exception(io_context& ctx, std::exception_ptr eptr) {
  if (!eptr) return;
  try {
    std::rethrow_exception(eptr);
  } catch (std::exception const& e) {
    ADD_FAILURE() << "Unhandled exception in spawned coroutine: " << e.what();
  } catch (...) {
    ADD_FAILURE() << "Unhandled unknown exception in spawned coroutine";
  }
  ctx.stop();
}

// Helper function to create a simple awaitable that returns a value
auto make_value_task(int value) -> awaitable<int> {
  co_return value;
}

// Helper function to create a void awaitable
auto make_void_task() -> awaitable<void> {
  co_return;
}

// Helper function to create a delayed task using io_context
auto make_delayed_task(io_context& ctx, int value, std::chrono::milliseconds delay) -> awaitable<int> {
  std::atomic<bool> timer_fired{false};
  ctx.schedule_timer(delay, [&]() {
    timer_fired.store(true);
  });

  // Busy wait for timer (simple approach for testing)
  while (!timer_fired.load()) {
    co_await make_void_task();
  }

  co_return value;
}

TEST(WhenAllTest, BasicTwoTasks) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    auto [a, b] = co_await when_all(make_value_task(10), make_value_task(20));
    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 20);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAllTest, ThreeTasks) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    auto [a, b, c] = co_await when_all(
      make_value_task(1),
      make_value_task(2),
      make_value_task(3)
    );
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
    EXPECT_EQ(c, 3);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAllTest, SingleTask) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    auto [result] = co_await when_all(make_value_task(42));
    EXPECT_EQ(result, 42);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAllTest, VoidTasks) {
  io_context ctx;
  std::atomic<int> counter{0};
  std::atomic<bool> executed{false};

  auto increment_task = [&]() -> awaitable<void> {
    counter.fetch_add(1);
    co_return;
  };

  co_spawn(ctx, [&, increment_task]() -> awaitable<void> {
    [[maybe_unused]] auto [a, b, c] = co_await when_all(
      increment_task(),
      increment_task(),
      increment_task()
    );
    // a, b, c are std::monostate
    EXPECT_EQ(counter.load(), 3);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
  EXPECT_EQ(counter.load(), 3);
}

TEST(WhenAllTest, MixedVoidAndNonVoid) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    auto [a, b, c] = co_await when_all(
      make_value_task(10),
      make_void_task(),
      make_value_task(20)
    );
    EXPECT_EQ(a, 10);
    // b is std::monostate (void result)
    EXPECT_EQ(c, 20);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAllTest, ExceptionInOneTask) {
  io_context ctx;
  std::atomic<bool> exception_caught{false};

  auto throwing_task = []() -> awaitable<int> {
    throw std::runtime_error("test exception");
    co_return 42;
  };

  co_spawn(ctx, [&, throwing_task]() -> awaitable<void> {
    try {
      [[maybe_unused]] auto [a, b] = co_await when_all(make_value_task(10), throwing_task());
      // Should not reach here
      exception_caught.store(false);
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ(e.what(), "test exception");
      exception_caught.store(true);
    }
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(exception_caught.load());
}

TEST(WhenAllTest, DifferentTypes) {
  io_context ctx;
  std::atomic<bool> executed{false};

  auto string_task = []() -> awaitable<std::string> {
    co_return "hello";
  };

  auto int_task = []() -> awaitable<int> {
    co_return 42;
  };

  auto double_task = []() -> awaitable<double> {
    co_return 3.14;
  };

  co_spawn(ctx, [&, string_task, int_task, double_task]() -> awaitable<void> {
    auto [s, i, d] = co_await when_all(string_task(), int_task(), double_task());
    EXPECT_EQ(s, "hello");
    EXPECT_EQ(i, 42);
    EXPECT_DOUBLE_EQ(d, 3.14);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAnyTest, BasicTwoTasks) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    auto [index, result] = co_await when_any(make_value_task(10), make_value_task(20));
    // One of them completed (we don't know which for simultaneous tasks)
    EXPECT_TRUE(index == 0 || index == 1);
    if (index == 0) {
      EXPECT_EQ(std::get<0>(result), 10);
    } else {
      EXPECT_EQ(std::get<1>(result), 20);
    }
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAnyTest, SingleTask) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    auto [index, result] = co_await when_any(make_value_task(42));
    EXPECT_EQ(index, 0);
    EXPECT_EQ(std::get<0>(result), 42);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAnyTest, VoidTasks) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    auto [index, result] = co_await when_any(make_void_task(), make_void_task());
    EXPECT_TRUE(index == 0 || index == 1);
    // Result is a variant with monostate values
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAnyTest, MixedTypes) {
  io_context ctx;
  std::atomic<bool> executed{false};

  auto string_task = []() -> awaitable<std::string> {
    co_return "hello";
  };

  auto int_task = []() -> awaitable<int> {
    co_return 42;
  };

  co_spawn(ctx, [&, string_task, int_task]() -> awaitable<void> {
    auto [index, result] = co_await when_any(string_task(), int_task());
    EXPECT_TRUE(index == 0 || index == 1);
    if (index == 0) {
      EXPECT_EQ(std::get<0>(result), "hello");
    } else {
      EXPECT_EQ(std::get<1>(result), 42);
    }
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAnyTest, ExceptionInFirstTask) {
  io_context ctx;
  std::atomic<bool> exception_caught{false};

  auto throwing_task = []() -> awaitable<int> {
    throw std::runtime_error("test exception");
    co_return 42;
  };

  co_spawn(ctx, [&, throwing_task]() -> awaitable<void> {
    try {
      // If throwing task completes first, exception is propagated
      auto [index, result] = co_await when_any(throwing_task(), make_value_task(10));
      // If we get here, the non-throwing task completed first
      EXPECT_EQ(index, 1);
      EXPECT_EQ(std::get<1>(result), 10);
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ(e.what(), "test exception");
      exception_caught.store(true);
    }
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  // Either the exception was caught or the other task won
  // We can't predict which happens first
}

TEST(WhenAnyTest, FirstToCompleteWins) {
  io_context ctx;
  std::atomic<bool> executed{false};
  std::atomic<int> slow_task_counter{0};

  auto fast_task = []() -> awaitable<int> {
    co_return 1;
  };

  auto slow_task = [&]() -> awaitable<int> {
    // This task increments counter even if it loses
    for (int i = 0; i < 10; ++i) {
      co_await make_void_task();
    }
    slow_task_counter.fetch_add(1);
    co_return 2;
  };

  co_spawn(ctx, [&, fast_task, slow_task]() -> awaitable<void> {
    auto [index, result] = co_await when_any(fast_task(), slow_task());
    // Fast task should win
    EXPECT_EQ(index, 0);
    EXPECT_EQ(std::get<0>(result), 1);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(IntegrationTest, WhenAllWithNestedTasks) {
  io_context ctx;
  std::atomic<bool> executed{false};

  auto nested_task = []() -> awaitable<int> {
    auto [a, b] = co_await when_all(make_value_task(5), make_value_task(10));
    co_return a + b;
  };

  co_spawn(ctx, [&, nested_task]() -> awaitable<void> {
    auto [x, y] = co_await when_all(nested_task(), make_value_task(20));
    EXPECT_EQ(x, 15);  // 5 + 10
    EXPECT_EQ(y, 20);
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(IntegrationTest, WhenAnyWithWhenAll) {
  io_context ctx;
  std::atomic<bool> executed{false};

  auto all_task = []() -> awaitable<int> {
    auto [a, b, c] = co_await when_all(
      make_value_task(1),
      make_value_task(2),
      make_value_task(3)
    );
    co_return a + b + c;
  };

  co_spawn(ctx, [&, all_task]() -> awaitable<void> {
    auto [index, result] = co_await when_any(all_task(), make_value_task(100));
    EXPECT_TRUE(index == 0 || index == 1);
    if (index == 0) {
      EXPECT_EQ(std::get<0>(result), 6);  // 1+2+3
    } else {
      EXPECT_EQ(std::get<1>(result), 100);
    }
    executed.store(true);
  }, [&](std::exception_ptr e) {
    fail_and_stop_on_exception(ctx, e);
  });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAllTest, VectorOfValueTasks) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    std::vector<awaitable<int>> tasks;
    tasks.push_back(make_value_task(10));
    tasks.push_back(make_value_task(20));
    tasks.push_back(make_value_task(30));

    auto results = co_await when_all(std::move(tasks));

    EXPECT_EQ(results.size(), 3);
    EXPECT_EQ(results[0], 10);
    EXPECT_EQ(results[1], 20);
    EXPECT_EQ(results[2], 30);
    executed.store(true);
  }, [&](std::exception_ptr e) { fail_and_stop_on_exception(ctx, e); });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAllTest, VectorOfVoidTasks) {
  io_context ctx;
  std::atomic<int> counter{0};
  std::atomic<bool> executed{false};

  auto increment_task = [&]() -> awaitable<void> {
    counter.fetch_add(1);
    co_return;
  };

  co_spawn(ctx, [&, increment_task]() -> awaitable<void> {
    std::vector<awaitable<void>> tasks;
    tasks.push_back(increment_task());
    tasks.push_back(increment_task());
    tasks.push_back(increment_task());

    auto results = co_await when_all(std::move(tasks));

    EXPECT_EQ(results.size(), 3);
    EXPECT_EQ(counter.load(), 3);
    executed.store(true);
  }, [&](std::exception_ptr e) { fail_and_stop_on_exception(ctx, e); });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAllTest, EmptyVector) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    std::vector<awaitable<int>> tasks;

    auto results = co_await when_all(std::move(tasks));

    EXPECT_EQ(results.size(), 0);
    executed.store(true);
  }, [&](std::exception_ptr e) { fail_and_stop_on_exception(ctx, e); });

  ctx.run();
  EXPECT_TRUE(executed.load());
}

TEST(WhenAllTest, VectorWithException) {
  io_context ctx;
  std::atomic<bool> exception_caught{false};

  auto throwing_task = []() -> awaitable<int> {
    throw std::runtime_error("test exception from vector");
    co_return 42;
  };

  co_spawn(ctx, [&, throwing_task]() -> awaitable<void> {
    try {
      std::vector<awaitable<int>> tasks;
      tasks.push_back(make_value_task(10));
      tasks.push_back(throwing_task());
      tasks.push_back(make_value_task(30));

      [[maybe_unused]] auto results = co_await when_all(std::move(tasks));
      // Should not reach here
      exception_caught.store(false);
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ(e.what(), "test exception from vector");
      exception_caught.store(true);
    }
  }, [&](std::exception_ptr e) { fail_and_stop_on_exception(ctx, e); });

  ctx.run();
  EXPECT_TRUE(exception_caught.load());
}

TEST(WhenAllTest, LargeVectorOfTasks) {
  io_context ctx;
  std::atomic<bool> executed{false};

  co_spawn(ctx, [&]() -> awaitable<void> {
    std::vector<awaitable<int>> tasks;
    for (int i = 0; i < 100; ++i) {
      tasks.push_back(make_value_task(i));
    }

    auto results = co_await when_all(std::move(tasks));

    EXPECT_EQ(results.size(), 100);
    for (int i = 0; i < 100; ++i) {
      EXPECT_EQ(results[i], i);
    }
    executed.store(true);
  }, [&](std::exception_ptr e) { fail_and_stop_on_exception(ctx, e); });

  ctx.run();
  EXPECT_TRUE(executed.load());
}
