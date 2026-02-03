cmake_minimum_required(VERSION 3.15)
project(iocoro VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(IOCORO_BUILD_TESTS "Build tests" OFF)
option(IOCORO_ENABLE_WARNINGS "Enable warning flags for iocoro tests/examples" ${PROJECT_IS_TOP_LEVEL})
option(IOCORO_ENABLE_URING "Enable io_uring backend if liburing is available" OFF)
option(IOCORO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(IOCORO_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(IOCORO_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})

# NOTE: For installed packages, io_uring is opt-in at consumer configure time:
# set(IOCORO_ENABLE_URING ON) before find_package(iocoro).

add_library(iocoro INTERFACE)
add_library(iocoro::iocoro ALIAS iocoro)
target_include_directories(iocoro INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(iocoro INTERFACE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(iocoro INTERFACE Threads::Threads)

if(IOCORO_ENABLE_URING)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBURING QUIET liburing)
    endif()

    find_path(LIBURING_INCLUDE_DIR NAMES liburing.h HINTS ${LIBURING_INCLUDE_DIRS})
    find_library(LIBURING_LIBRARY NAMES uring HINTS ${LIBURING_LIBRARY_DIRS})

    if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
        message(STATUS "liburing found - io_uring support enabled")
        message(STATUS "  Include: ${LIBURING_INCLUDE_DIR}")
        message(STATUS "  Library: ${LIBURING_LIBRARY}")

        if(NOT TARGET liburing::liburing)
            add_library(liburing::liburing UNKNOWN IMPORTED)
            set_target_properties(liburing::liburing PROPERTIES
                IMPORTED_LOCATION "${LIBURING_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${LIBURING_INCLUDE_DIR}"
            )
        endif()

        target_link_libraries(iocoro INTERFACE liburing::liburing)
        # Backend selection is controlled by preprocessor macros (header-only friendly).
        target_compile_definitions(iocoro INTERFACE IOCORO_BACKEND_URING)
    else()
        message(STATUS "liburing not found - using epoll")
    endif()
else()
    message(STATUS "IOCORO_ENABLE_URING=OFF - using default backend selection (epoll)")
endif()

if(IOCORO_ENABLE_WARNINGS)
    add_library(iocoro_warnings INTERFACE)
    target_compile_options(iocoro_warnings INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
    )
endif()

if(IOCORO_ENABLE_ASAN)
    target_compile_options(iocoro INTERFACE
        -g
        -fno-omit-frame-pointer
        -fsanitize=address
    )
    target_link_options(iocoro INTERFACE
        -fsanitize=address
    )
    message(STATUS "iocoro: AddressSanitizer is ENABLED")
endif()

if(IOCORO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(IOCORO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(IOCORO_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

install(TARGETS iocoro EXPORT iocoroTargets)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT iocoroTargets
    NAMESPACE iocoro::
    FILE iocoroTargets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iocoro"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iocoroConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/iocoroConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iocoro"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/iocoroConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY ExactVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/iocoroConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iocoroConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iocoro"
)
