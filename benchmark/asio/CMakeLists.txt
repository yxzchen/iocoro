find_package(Boost REQUIRED)

add_executable(asio_tcp_server tcp_server.cpp)
target_link_libraries(asio_tcp_server PRIVATE Threads::Threads)
target_compile_features(asio_tcp_server PRIVATE cxx_std_20)
target_include_directories(asio_tcp_server PRIVATE ${Boost_INCLUDE_DIRS})

add_executable(asio_tcp_client tcp_client.cpp)
target_link_libraries(asio_tcp_client PRIVATE Threads::Threads)
target_compile_features(asio_tcp_client PRIVATE cxx_std_20)
target_include_directories(asio_tcp_client PRIVATE ${Boost_INCLUDE_DIRS})
