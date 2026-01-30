add_executable(iocoro_tcp_server tcp_server.cpp)
target_link_libraries(iocoro_tcp_server PRIVATE iocoro::iocoro Threads::Threads)
target_compile_features(iocoro_tcp_server PRIVATE cxx_std_20)

add_executable(iocoro_tcp_client tcp_client.cpp)
target_link_libraries(iocoro_tcp_client PRIVATE iocoro::iocoro Threads::Threads)
target_compile_features(iocoro_tcp_client PRIVATE cxx_std_20)
