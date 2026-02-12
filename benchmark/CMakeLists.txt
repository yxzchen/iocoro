find_package(Threads REQUIRED)
find_package(Boost REQUIRED)

add_executable(iocoro_tcp_roundtrip apps/iocoro_tcp_roundtrip.cpp)
target_link_libraries(iocoro_tcp_roundtrip PRIVATE iocoro::iocoro Threads::Threads)
target_compile_features(iocoro_tcp_roundtrip PRIVATE cxx_std_20)
target_compile_options(iocoro_tcp_roundtrip PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)

add_executable(asio_tcp_roundtrip apps/asio_tcp_roundtrip.cpp)
target_link_libraries(asio_tcp_roundtrip PRIVATE Threads::Threads)
target_compile_features(asio_tcp_roundtrip PRIVATE cxx_std_20)
target_compile_options(asio_tcp_roundtrip PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
target_include_directories(asio_tcp_roundtrip PRIVATE ${Boost_INCLUDE_DIRS})

add_executable(iocoro_tcp_connect_accept apps/iocoro_tcp_connect_accept.cpp)
target_link_libraries(iocoro_tcp_connect_accept PRIVATE iocoro::iocoro Threads::Threads)
target_compile_features(iocoro_tcp_connect_accept PRIVATE cxx_std_20)
target_compile_options(iocoro_tcp_connect_accept PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)

add_executable(asio_tcp_connect_accept apps/asio_tcp_connect_accept.cpp)
target_link_libraries(asio_tcp_connect_accept PRIVATE Threads::Threads)
target_compile_features(asio_tcp_connect_accept PRIVATE cxx_std_20)
target_compile_options(asio_tcp_connect_accept PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
target_include_directories(asio_tcp_connect_accept PRIVATE ${Boost_INCLUDE_DIRS})
