find_package(Threads REQUIRED)
find_package(Boost REQUIRED)

add_executable(iocoro_tcp_echo iocoro_tcp_echo.cpp)
target_link_libraries(iocoro_tcp_echo PRIVATE iocoro::iocoro Threads::Threads)
target_compile_features(iocoro_tcp_echo PRIVATE cxx_std_20)
target_compile_options(iocoro_tcp_echo PRIVATE -fcoroutines)

add_executable(asio_tcp_echo asio_tcp_echo.cpp)
target_link_libraries(asio_tcp_echo PRIVATE Threads::Threads)
target_compile_features(asio_tcp_echo PRIVATE cxx_std_20)
target_compile_options(asio_tcp_echo PRIVATE -fcoroutines)
target_include_directories(asio_tcp_echo PRIVATE ${Boost_INCLUDE_DIRS})
