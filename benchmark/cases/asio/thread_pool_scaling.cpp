#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>

int main(int argc, char* argv[]) {
  int workers = 1;
  std::uint64_t tasks = 1;
  if (argc >= 3) {
    workers = std::stoi(argv[1]);
    tasks = static_cast<std::uint64_t>(std::stoull(argv[2]));
  }
  if (workers <= 0) {
    std::cerr << "asio_thread_pool_scaling: workers must be > 0\n";
    return 1;
  }
  if (tasks == 0) {
    std::cerr << "asio_thread_pool_scaling: tasks must be > 0\n";
    return 1;
  }

  boost::asio::thread_pool pool{static_cast<std::size_t>(workers)};
  auto ex = pool.get_executor();

  std::mutex mtx;
  std::condition_variable cv;
  std::atomic<std::uint64_t> remaining{tasks};
  bool done = false;

  auto const start = std::chrono::steady_clock::now();

  for (std::uint64_t i = 0; i < tasks; ++i) {
    boost::asio::post(ex, [&] {
      if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        {
          std::scoped_lock lk{mtx};
          done = true;
        }
        cv.notify_one();
      }
    });
  }

  {
    std::unique_lock lk{mtx};
    cv.wait(lk, [&] { return done; });
  }

  auto const end = std::chrono::steady_clock::now();
  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const ops_s = elapsed_s > 0.0 ? static_cast<double>(tasks) / elapsed_s : 0.0;
  auto const avg_us =
    elapsed_s > 0.0 ? (elapsed_s * 1'000'000.0) / static_cast<double>(tasks) : 0.0;

  pool.join();

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "asio_thread_pool_scaling"
            << " workers=" << workers << " tasks=" << tasks << " elapsed_s=" << elapsed_s
            << " ops_s=" << ops_s << " avg_us=" << avg_us << "\n";
  return 0;
}
