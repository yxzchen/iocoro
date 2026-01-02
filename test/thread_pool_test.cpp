#include <gtest/gtest.h>

#include <iocoro/iocoro.hpp>
#include <iocoro/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include <stdexcept>
#include <unordered_map>

namespace {

using namespace std::chrono_literals;

// ========== Basic Functionality Tests ==========

TEST(thread_pool, post_runs_on_multiple_threads) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::mutex m;
  std::condition_variable cv;
  std::unordered_set<std::thread::id> threads;

  std::atomic<int> remaining{200};

  for (int i = 0; i < 200; ++i) {
    ex.post([&] {
      {
        std::scoped_lock lk{m};
        threads.insert(std::this_thread::get_id());
      }
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        cv.notify_one();
      }
    });
  }

  {
    std::unique_lock lk{m};
    cv.wait_for(lk, 2s, [&] { return remaining.load(std::memory_order_acquire) == 0; });
  }

  EXPECT_EQ(remaining.load(std::memory_order_acquire), 0);

  {
    std::scoped_lock lk{m};
    EXPECT_GT(threads.size(), 1u);
  }
}

TEST(thread_pool, single_thread_pool) {
  iocoro::thread_pool pool{1};
  auto ex = pool.get_executor();

  std::atomic<int> counter{0};
  std::promise<void> done;
  auto fut = done.get_future();

  const int num_tasks = 100;
  for (int i = 0; i < num_tasks; ++i) {
    ex.post([&, i] {
      counter.fetch_add(1, std::memory_order_relaxed);
      if (i == num_tasks - 1) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(counter.load(), num_tasks);
}

TEST(thread_pool, size_returns_thread_count) {
  iocoro::thread_pool pool{8};
  EXPECT_EQ(pool.size(), 8u);
}

TEST(thread_pool, executes_large_number_of_tasks) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::atomic<int> completed{0};
  const int num_tasks = 10000;

  std::promise<void> done;
  auto fut = done.get_future();

  for (int i = 0; i < num_tasks; ++i) {
    ex.post([&, i] {
      std::this_thread::sleep_for(1us);  // Simulate some work
      if (completed.fetch_add(1, std::memory_order_acq_rel) == num_tasks - 1) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(completed.load(), num_tasks);
}

TEST(thread_pool, tasks_with_return_values) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::vector<std::promise<int>> promises(10);
  std::vector<std::future<int>> futures;

  for (auto& p : promises) {
    futures.push_back(p.get_future());
  }

  for (int i = 0; i < 10; ++i) {
    ex.post([&promises, i] {
      std::this_thread::sleep_for(10ms);
      promises[i].set_value(i * i);
    });
  }

  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(futures[i].wait_for(2s), std::future_status::ready);
    EXPECT_EQ(futures[i].get(), i * i);
  }
}

// ========== Stop and Destruction Tests ==========

TEST(thread_pool, stop_prevents_new_tasks) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  pool.stop();

  std::atomic<bool> task_executed{false};
  ex.post([&] {
    task_executed.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(task_executed.load());
}

TEST(thread_pool, stop_is_idempotent) {
  iocoro::thread_pool pool{2};

  pool.stop();
  pool.stop();  // Second call should be safe
  pool.stop();  // Third call should be safe

  pool.join();
  pool.join();  // join should also be idempotent
}

TEST(thread_pool, executor_stopped_returns_true_after_stop) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  EXPECT_FALSE(ex.stopped());

  pool.stop();

  EXPECT_TRUE(ex.stopped());
}

TEST(thread_pool, destructor_completes_pending_tasks) {
  std::atomic<int> completed{0};

  {
    iocoro::thread_pool pool{2};
    auto ex = pool.get_executor();

    for (int i = 0; i < 100; ++i) {
      ex.post([&] {
        completed.fetch_add(1, std::memory_order_relaxed);
      });
    }

    // Destructor should wait for tasks to complete
  }

  // All tasks should have executed
  EXPECT_EQ(completed.load(), 100);
}

TEST(thread_pool, can_stop_and_join_explicitly) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> completed{0};

  for (int i = 0; i < 10; ++i) {
    ex.post([&] {
      completed.fetch_add(1, std::memory_order_relaxed);
    });
  }

  pool.stop();
  pool.join();

  EXPECT_EQ(completed.load(), 10);
}

// ========== Work Guard Tests ==========

TEST(thread_pool, work_guard_basic_usage) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  // Creating work guard should succeed
  auto guard = iocoro::make_work_guard(ex);

  std::atomic<int> counter{0};
  std::promise<void> done;
  auto fut = done.get_future();

  ex.post([&] {
    counter.fetch_add(1);
    done.set_value();
  });

  EXPECT_EQ(fut.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(counter.load(), 1);

  // Release guard
  guard.reset();
}

TEST(thread_pool, multiple_work_guards_basic) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  auto guard1 = iocoro::make_work_guard(ex);
  auto guard2 = iocoro::make_work_guard(ex);
  auto guard3 = iocoro::make_work_guard(ex);

  std::atomic<int> counter{0};
  std::promise<void> done;
  auto fut = done.get_future();

  ex.post([&] {
    counter.fetch_add(1);
    done.set_value();
  });

  EXPECT_EQ(fut.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(counter.load(), 1);

  // Release all guards
  guard1.reset();
  guard2.reset();
  guard3.reset();
}

TEST(thread_pool, work_guard_prevents_thread_exit_after_stop) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> counter{0};
  std::promise<void> done;
  auto fut = done.get_future();

  // Post tasks BEFORE stopping
  for (int i = 0; i < 10; ++i) {
    ex.post([&, i] {
      std::this_thread::sleep_for(50ms);  // Simulate work
      if (counter.fetch_add(1) == 9) {
        done.set_value();
      }
    });
  }

  // Create work guard BEFORE stop
  auto guard = iocoro::make_work_guard(ex);

  // Stop the pool - but guard should keep threads alive to finish queued tasks
  pool.stop();

  // Tasks should still execute because of work guard
  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(counter.load(), 10);

  // Release guard to allow threads to exit
  guard.reset();
  pool.join();
}

TEST(thread_pool, work_guard_allows_exit_when_no_tasks_remain) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> counter{0};

  // Post and complete some tasks
  for (int i = 0; i < 5; ++i) {
    ex.post([&] {
      counter.fetch_add(1);
    });
  }

  std::this_thread::sleep_for(100ms);  // Wait for tasks to complete
  EXPECT_EQ(counter.load(), 5);

  // Now create guard and stop
  auto guard = iocoro::make_work_guard(ex);
  pool.stop();

  // Release guard - threads should exit since no tasks remain
  guard.reset();
  pool.join();
}

// ========== Executor Functionality Tests ==========

TEST(thread_pool, dispatch_runs_inline_on_same_executor) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::promise<void> done;
  auto fut = done.get_future();

  std::thread::id task_thread_id;
  std::thread::id dispatch_thread_id;

  ex.post([&] {
    task_thread_id = std::this_thread::get_id();

    // dispatch should execute inline on same thread
    ex.dispatch([&] {
      dispatch_thread_id = std::this_thread::get_id();
      done.set_value();
    });
  });

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(task_thread_id, dispatch_thread_id);
}

TEST(thread_pool, dispatch_posts_on_different_executor) {
  iocoro::thread_pool pool1{1};
  iocoro::thread_pool pool2{1};

  auto ex1 = pool1.get_executor();
  auto ex2 = pool2.get_executor();

  std::promise<void> done;
  auto fut = done.get_future();

  std::thread::id thread1_id;
  std::thread::id thread2_id;

  ex1.post([&] {
    thread1_id = std::this_thread::get_id();

    // dispatch to different executor should post
    ex2.dispatch([&] {
      thread2_id = std::this_thread::get_id();
      done.set_value();
    });
  });

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_NE(thread1_id, thread2_id);
}

TEST(thread_pool, default_constructed_executor_is_empty) {
  iocoro::thread_pool::executor_type ex;

  EXPECT_FALSE(ex);
  EXPECT_TRUE(ex.stopped());

  // post to empty executor should be safe (does nothing)
  std::atomic<bool> executed{false};
  ex.post([&] { executed.store(true); });

  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(executed.load());
}

TEST(thread_pool, multiple_executors_share_same_pool) {
  iocoro::thread_pool pool{4};

  auto ex1 = pool.get_executor();
  auto ex2 = pool.get_executor();
  auto ex3 = pool.get_executor();

  std::atomic<int> counter{0};
  std::promise<void> done;
  auto fut = done.get_future();

  const int tasks_per_executor = 100;

  for (int i = 0; i < tasks_per_executor; ++i) {
    ex1.post([&] {
      if (counter.fetch_add(1) == tasks_per_executor * 3 - 1) {
        done.set_value();
      }
    });
    ex2.post([&] {
      if (counter.fetch_add(1) == tasks_per_executor * 3 - 1) {
        done.set_value();
      }
    });
    ex3.post([&] {
      if (counter.fetch_add(1) == tasks_per_executor * 3 - 1) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(counter.load(), tasks_per_executor * 3);
}

// ========== Concurrency Safety Tests ==========

TEST(thread_pool, concurrent_post_from_multiple_threads) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::atomic<int> counter{0};
  const int threads = 10;
  const int tasks_per_thread = 100;

  std::vector<std::thread> post_threads;
  std::promise<void> done;
  auto fut = done.get_future();

  for (int t = 0; t < threads; ++t) {
    post_threads.emplace_back([&] {
      for (int i = 0; i < tasks_per_thread; ++i) {
        ex.post([&] {
          if (counter.fetch_add(1, std::memory_order_acq_rel) == threads * tasks_per_thread - 1) {
            done.set_value();
          }
        });
      }
    });
  }

  for (auto& t : post_threads) {
    t.join();
  }

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(counter.load(), threads * tasks_per_thread);
}

TEST(thread_pool, concurrent_stop_and_post) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::atomic<bool> keep_posting{true};
  std::atomic<int> posted{0};
  std::atomic<int> executed{0};

  std::thread poster([&] {
    while (keep_posting.load()) {
      ex.post([&] {
        executed.fetch_add(1, std::memory_order_relaxed);
      });
      posted.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(1us);
    }
  });

  std::this_thread::sleep_for(50ms);
  pool.stop();
  keep_posting.store(false);

  poster.join();
  pool.join();

  // Tasks posted before stop should be executed, those after may not
  EXPECT_GT(executed.load(), 0);
}

TEST(thread_pool, tasks_can_post_more_tasks) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::atomic<int> counter{0};
  std::promise<void> done;
  auto fut = done.get_future();

  const int total_tasks = 100;

  // Recursively post tasks
  std::function<void(int)> recursive_post;
  recursive_post = [&](int remaining) {
    if (remaining == 0) {
      done.set_value();
      return;
    }

    ex.post([&, remaining] {
      counter.fetch_add(1, std::memory_order_relaxed);
      recursive_post(remaining - 1);
    });
  };

  recursive_post(total_tasks);

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(counter.load(), total_tasks);
}

// ========== Exception Handling Tests ==========

TEST(thread_pool, exception_in_task_is_swallowed_by_default) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> before_exception{0};
  std::atomic<int> after_exception{0};
  std::promise<void> done;
  auto fut = done.get_future();

  // Task before exception
  ex.post([&] {
    before_exception.fetch_add(1);
  });

  // Task that throws exception
  ex.post([&] {
    throw std::runtime_error("test exception");
  });

  // Tasks after exception should still execute
  for (int i = 0; i < 10; ++i) {
    ex.post([&, i] {
      if (after_exception.fetch_add(1) == 9) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(before_exception.load(), 1);
  EXPECT_EQ(after_exception.load(), 10);
}

TEST(thread_pool, exception_handler_is_called) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> exception_count{0};
  std::promise<void> done;
  auto fut = done.get_future();

  // Set exception handler
  pool.set_exception_handler([&](std::exception_ptr eptr) {
    exception_count.fetch_add(1);

    // Verify we can rethrow and catch the exception
    try {
      if (eptr) {
        std::rethrow_exception(eptr);
      }
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ(e.what(), "test exception");
    }
  });

  // Post task that throws
  ex.post([&] {
    throw std::runtime_error("test exception");
  });

  // Post task to signal completion
  ex.post([&] {
    done.set_value();
  });

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(exception_count.load(), 1);
}

TEST(thread_pool, multiple_exceptions_are_all_handled) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::atomic<int> exception_count{0};
  std::atomic<int> normal_tasks{0};
  std::promise<void> done;
  auto fut = done.get_future();

  // Set exception handler
  pool.set_exception_handler([&](std::exception_ptr eptr) {
    exception_count.fetch_add(1);
  });

  // Post mix of normal and throwing tasks
  for (int i = 0; i < 100; ++i) {
    ex.post([&, i] {
      if (i % 10 == 0) {
        throw std::runtime_error("exception from task " + std::to_string(i));
      }
      if (normal_tasks.fetch_add(1) == 89) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(normal_tasks.load(), 90);
  EXPECT_EQ(exception_count.load(), 10);
}

TEST(thread_pool, exception_handler_can_be_changed) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> handler1_count{0};
  std::atomic<int> handler2_count{0};

  // Set first handler
  pool.set_exception_handler([&](std::exception_ptr) {
    handler1_count.fetch_add(1);
  });

  std::promise<void> done1;
  auto fut1 = done1.get_future();

  ex.post([&] { throw std::runtime_error("first"); });
  ex.post([&] { done1.set_value(); });

  EXPECT_EQ(fut1.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(handler1_count.load(), 1);
  EXPECT_EQ(handler2_count.load(), 0);

  // Change handler
  pool.set_exception_handler([&](std::exception_ptr) {
    handler2_count.fetch_add(1);
  });

  std::promise<void> done2;
  auto fut2 = done2.get_future();

  ex.post([&] { throw std::runtime_error("second"); });
  ex.post([&] { done2.set_value(); });

  EXPECT_EQ(fut2.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(handler1_count.load(), 1);
  EXPECT_EQ(handler2_count.load(), 1);
}

TEST(thread_pool, exception_handler_exception_is_swallowed) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> handler_called{0};
  std::promise<void> done;
  auto fut = done.get_future();

  // Set handler that itself throws
  pool.set_exception_handler([&](std::exception_ptr) {
    handler_called.fetch_add(1);
    throw std::runtime_error("exception in handler");
  });

  ex.post([&] { throw std::runtime_error("task exception"); });

  // This task should still execute despite handler throwing
  ex.post([&] { done.set_value(); });

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(handler_called.load(), 1);
}

TEST(thread_pool, different_exception_types_are_handled) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> runtime_error_count{0};
  std::atomic<int> logic_error_count{0};
  std::atomic<int> other_error_count{0};
  std::promise<void> done;
  auto fut = done.get_future();

  pool.set_exception_handler([&](std::exception_ptr eptr) {
    try {
      if (eptr) {
        std::rethrow_exception(eptr);
      }
    } catch (const std::runtime_error&) {
      runtime_error_count.fetch_add(1);
    } catch (const std::logic_error&) {
      logic_error_count.fetch_add(1);
    } catch (...) {
      other_error_count.fetch_add(1);
    }
  });

  ex.post([&] { throw std::runtime_error("runtime"); });
  ex.post([&] { throw std::logic_error("logic"); });
  ex.post([&] { throw 42; });
  ex.post([&] { done.set_value(); });

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(runtime_error_count.load(), 1);
  EXPECT_EQ(logic_error_count.load(), 1);
  EXPECT_EQ(other_error_count.load(), 1);
}

// ========== Task Chaining and Nesting Tests ==========

TEST(thread_pool, chained_tasks) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::atomic<int> step{0};
  std::promise<void> done;
  auto fut = done.get_future();

  // Task 1
  ex.post([&] {
    EXPECT_EQ(step.fetch_add(1), 0);

    // Task 2
    ex.post([&] {
      EXPECT_EQ(step.fetch_add(1), 1);

      // Task 3
      ex.post([&] {
        EXPECT_EQ(step.fetch_add(1), 2);
        done.set_value();
      });
    });
  });

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(step.load(), 3);
}

// ========== Coroutine Integration Tests ==========

TEST(thread_pool, co_spawn_accepts_thread_pool_executor) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  auto done = std::make_shared<std::promise<void>>();
  auto fut = done->get_future();

  std::atomic<bool> saw_executor{false};

  iocoro::co_spawn(
    ex,
    [done, &saw_executor]() -> iocoro::awaitable<void> {
      auto current = co_await iocoro::this_coro::executor;
      if (current) {
        saw_executor.store(true, std::memory_order_release);
      }
      done->set_value();
      co_return;
    },
    iocoro::detached);

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  fut.get();
  EXPECT_TRUE(saw_executor.load(std::memory_order_acquire));
}

TEST(thread_pool, multiple_coroutines_on_pool) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::atomic<int> counter{0};
  auto done = std::make_shared<std::promise<void>>();
  auto fut = done->get_future();

  const int num_coros = 50;

  for (int i = 0; i < num_coros; ++i) {
    iocoro::co_spawn(
      ex,
      [&counter, done, i, num_coros]() -> iocoro::awaitable<void> {
        // Simply verify coroutines execute on pool
        if (counter.fetch_add(1, std::memory_order_acq_rel) == num_coros - 1) {
          done->set_value();
        }
        co_return;
      },
      iocoro::detached);
  }

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(counter.load(), num_coros);
}

// ========== Performance and Load Balancing Tests ==========

TEST(thread_pool, load_balancing_across_threads) {
  iocoro::thread_pool pool{8};
  auto ex = pool.get_executor();

  std::mutex m;
  std::unordered_map<std::thread::id, int> thread_task_counts;
  std::promise<void> done;
  auto fut = done.get_future();

  std::atomic<int> remaining{1000};

  for (int i = 0; i < 1000; ++i) {
    ex.post([&] {
      std::this_thread::sleep_for(1ms);  // Simulate work

      {
        std::scoped_lock lk{m};
        thread_task_counts[std::this_thread::get_id()]++;
      }

      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(5s), std::future_status::ready);

  {
    std::scoped_lock lk{m};
    // Should have multiple threads participating
    EXPECT_GT(thread_task_counts.size(), 1u);

    // Each thread should have executed some tasks (rough load balancing check)
    for (const auto& [tid, count] : thread_task_counts) {
      EXPECT_GT(count, 0);
    }
  }
}

TEST(thread_pool, mixed_short_and_long_tasks) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::atomic<int> short_tasks{0};
  std::atomic<int> long_tasks{0};
  std::promise<void> done;
  auto fut = done.get_future();

  const int total_tasks = 100;
  std::atomic<int> completed{0};

  for (int i = 0; i < total_tasks; ++i) {
    if (i % 10 == 0) {
      // Long task
      ex.post([&] {
        std::this_thread::sleep_for(50ms);
        long_tasks.fetch_add(1);
        if (completed.fetch_add(1) == total_tasks - 1) {
          done.set_value();
        }
      });
    } else {
      // Short task
      ex.post([&] {
        std::this_thread::sleep_for(1ms);
        short_tasks.fetch_add(1);
        if (completed.fetch_add(1) == total_tasks - 1) {
          done.set_value();
        }
      });
    }
  }

  EXPECT_EQ(fut.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(short_tasks.load(), 90);
  EXPECT_EQ(long_tasks.load(), 10);
}

// ========== Edge Cases Tests ==========

TEST(thread_pool, empty_lambda) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::promise<void> done;
  auto fut = done.get_future();

  ex.post([&] {
    done.set_value();
  });

  EXPECT_EQ(fut.wait_for(1s), std::future_status::ready);
}

TEST(thread_pool, capture_by_value) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  std::promise<int> done;
  auto fut = done.get_future();

  int value = 42;
  ex.post([value, &done] {
    done.set_value(value);
  });

  EXPECT_EQ(fut.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(fut.get(), 42);
}

TEST(thread_pool, capture_by_move) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  auto ptr = std::make_unique<int>(42);
  std::promise<int> done;
  auto fut = done.get_future();

  ex.post([ptr = std::move(ptr), &done]() mutable {
    done.set_value(*ptr);
  });

  EXPECT_EQ(fut.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(fut.get(), 42);
}

TEST(thread_pool, post_after_all_tasks_complete) {
  iocoro::thread_pool pool{2};
  auto ex = pool.get_executor();

  // First batch of tasks
  std::promise<void> first_batch;
  auto fut1 = first_batch.get_future();

  std::atomic<int> counter{0};
  for (int i = 0; i < 10; ++i) {
    ex.post([&, i] {
      if (counter.fetch_add(1) == 9) {
        first_batch.set_value();
      }
    });
  }

  EXPECT_EQ(fut1.wait_for(1s), std::future_status::ready);

  // Second batch of tasks
  std::promise<void> second_batch;
  auto fut2 = second_batch.get_future();

  counter.store(0);
  for (int i = 0; i < 10; ++i) {
    ex.post([&, i] {
      if (counter.fetch_add(1) == 9) {
        second_batch.set_value();
      }
    });
  }

  EXPECT_EQ(fut2.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(counter.load(), 10);
}

TEST(thread_pool, stress_test_rapid_task_submission) {
  iocoro::thread_pool pool{8};
  auto ex = pool.get_executor();

  std::atomic<int> counter{0};
  const int num_tasks = 50000;
  std::promise<void> done;
  auto fut = done.get_future();

  for (int i = 0; i < num_tasks; ++i) {
    ex.post([&] {
      if (counter.fetch_add(1, std::memory_order_acq_rel) == num_tasks - 1) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(counter.load(), num_tasks);
}

TEST(thread_pool, interleaved_post_and_dispatch) {
  iocoro::thread_pool pool{4};
  auto ex = pool.get_executor();

  std::atomic<int> post_count{0};
  std::atomic<int> dispatch_count{0};
  std::promise<void> done;
  auto fut = done.get_future();

  const int iterations = 50;

  for (int i = 0; i < iterations; ++i) {
    ex.post([&, i] {
      post_count.fetch_add(1);

      ex.dispatch([&, i] {
        if (dispatch_count.fetch_add(1) == iterations - 1) {
          done.set_value();
        }
      });
    });
  }

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(post_count.load(), iterations);
  EXPECT_EQ(dispatch_count.load(), iterations);
}

TEST(thread_pool, executor_copy_semantics) {
  iocoro::thread_pool pool{2};
  auto ex1 = pool.get_executor();
  auto ex2 = ex1;  // Copy
  auto ex3 = std::move(ex2);  // Move

  std::atomic<int> counter{0};
  std::promise<void> done;
  auto fut = done.get_future();

  ex1.post([&] { counter.fetch_add(1); });
  ex3.post([&] {
    counter.fetch_add(1);
    if (counter.load() == 2) {
      done.set_value();
    }
  });

  EXPECT_EQ(fut.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(counter.load(), 2);
}

TEST(thread_pool, verify_fifo_ordering_single_thread) {
  iocoro::thread_pool pool{1};  // Single thread ensures ordering
  auto ex = pool.get_executor();

  std::mutex m;
  std::vector<int> execution_order;
  std::promise<void> done;
  auto fut = done.get_future();

  const int num_tasks = 100;

  for (int i = 0; i < num_tasks; ++i) {
    ex.post([&, i] {
      std::scoped_lock lk{m};
      execution_order.push_back(i);
      if (i == num_tasks - 1) {
        done.set_value();
      }
    });
  }

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);

  // Verify execution order matches submission order
  EXPECT_EQ(execution_order.size(), static_cast<size_t>(num_tasks));
  for (int i = 0; i < num_tasks; ++i) {
    EXPECT_EQ(execution_order[i], i);
  }
}

TEST(thread_pool, nested_executors_from_different_pools) {
  iocoro::thread_pool pool1{2};
  iocoro::thread_pool pool2{2};

  auto ex1 = pool1.get_executor();
  auto ex2 = pool2.get_executor();

  std::atomic<int> counter{0};
  std::promise<void> done;
  auto fut = done.get_future();

  ex1.post([&, ex2] {
    counter.fetch_add(1);

    ex2.post([&, ex1] {
      counter.fetch_add(1);

      ex1.post([&] {
        counter.fetch_add(1);
        done.set_value();
      });
    });
  });

  EXPECT_EQ(fut.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(counter.load(), 3);
}

}  // namespace
