cmake_minimum_required(VERSION 3.15)
project(iocoro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

find_package(fmt REQUIRED)

# Optional liburing for io_uring support
find_path(LIBURING_INCLUDE_DIR liburing.h)
find_library(LIBURING_LIBRARY uring)

if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
    message(STATUS "liburing found - io_uring support enabled")
    message(STATUS "  Include: ${LIBURING_INCLUDE_DIR}")
    message(STATUS "  Library: ${LIBURING_LIBRARY}")
    set(LIBURING_FOUND TRUE)
else()
    message(STATUS "liburing not found - using epoll only")
    set(LIBURING_FOUND FALSE)
endif()

add_library(iocoro INTERFACE)
target_include_directories(iocoro INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(iocoro INTERFACE cxx_std_20)
target_link_libraries(iocoro INTERFACE pthread fmt::fmt)

if(LIBURING_FOUND)
    target_include_directories(iocoro INTERFACE ${LIBURING_INCLUDE_DIR})
    target_link_libraries(iocoro INTERFACE ${LIBURING_LIBRARY})
    target_compile_definitions(iocoro INTERFACE IOCORO_USE_URING)
endif()

option(IOCORO_BUILD_TESTS "Build tests" OFF)

if(IOCORO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
