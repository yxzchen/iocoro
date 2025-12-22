find_package(GTest REQUIRED)

macro(make_test TEST_NAME)
  add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
  target_compile_options(${TEST_NAME} PRIVATE -fcoroutines)
  target_link_libraries(${TEST_NAME} PRIVATE
    iocoro
    GTest::gtest
    GTest::gtest_main
  )
  if(TARGET iocoro_warnings)
    target_link_libraries(${TEST_NAME} PRIVATE iocoro_warnings)
  endif()
  gtest_discover_tests(${TEST_NAME})
endmacro()

make_test(io_context_test)
