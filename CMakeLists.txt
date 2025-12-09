cmake_minimum_required(VERSION 3.20)
project(ioxz VERSION 1.0.0 LANGUAGES CXX)

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
    add_compile_definitions(IOXZ_HAS_URING)
else()
    message(STATUS "liburing not found - using epoll only")
    set(LIBURING_FOUND FALSE)
endif()

add_library(ioxz INTERFACE)
target_include_directories(ioxz INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(ioxz INTERFACE cxx_std_20)
target_link_libraries(ioxz INTERFACE pthread fmt::fmt)

if(LIBURING_FOUND)
    target_include_directories(ioxz INTERFACE ${LIBURING_INCLUDE_DIR})
    target_link_libraries(ioxz INTERFACE ${LIBURING_LIBRARY})
endif()

option(BUILD_TESTS "Build tests" ON)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
