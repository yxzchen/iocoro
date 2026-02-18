find_package(Threads REQUIRED)
find_package(Boost REQUIRED)

function(add_iocoro_benchmark name)
  add_executable(iocoro_${name} cases/iocoro/${name}.cpp)
  target_link_libraries(iocoro_${name} PRIVATE iocoro::iocoro Threads::Threads)
  target_compile_features(iocoro_${name} PRIVATE cxx_std_20)
  target_compile_options(iocoro_${name} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
endfunction()

function(add_asio_benchmark name)
  add_executable(asio_${name} cases/asio/${name}.cpp)
  target_link_libraries(asio_${name} PRIVATE Threads::Threads)
  target_compile_features(asio_${name} PRIVATE cxx_std_20)
  target_compile_options(asio_${name} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
  target_include_directories(asio_${name} PRIVATE ${Boost_INCLUDE_DIRS})
endfunction()

set(BENCHMARK_NAMES
  tcp_roundtrip
  tcp_latency
  tcp_connect_accept
  tcp_throughput
  udp_send_receive
  timer_churn
  thread_pool_scaling
)

foreach(bench_name IN LISTS BENCHMARK_NAMES)
  add_iocoro_benchmark(${bench_name})
  add_asio_benchmark(${bench_name})
endforeach()
