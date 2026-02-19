find_package(GTest REQUIRED)

file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
set(COMPILE_ONLY_SMOKE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/compile_only_smoke.cpp")
list(REMOVE_ITEM TEST_SOURCES "${COMPILE_ONLY_SMOKE_SOURCE}")

foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_compile_options(${test_name} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
  target_link_libraries(${test_name} PRIVATE
    iocoro
    GTest::gtest
    GTest::gtest_main
  )
  if(TARGET iocoro_warnings)
    target_link_libraries(${test_name} PRIVATE iocoro_warnings)
  endif()
  gtest_discover_tests(${test_name})
endforeach()

add_library(iocoro_compile_only_smoke OBJECT ${COMPILE_ONLY_SMOKE_SOURCE})
target_compile_options(iocoro_compile_only_smoke PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
target_link_libraries(iocoro_compile_only_smoke PRIVATE iocoro)
if(TARGET iocoro_warnings)
  target_link_libraries(iocoro_compile_only_smoke PRIVATE iocoro_warnings)
endif()
