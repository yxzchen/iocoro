macro(make_example EXE_NAME)
  add_executable(${EXE_NAME} ${EXE_NAME}.cpp)
  target_compile_features(${EXE_NAME} PRIVATE cxx_std_20)
  target_compile_options(${EXE_NAME} PRIVATE -fcoroutines)
  target_link_libraries(${EXE_NAME} PRIVATE iocoro::iocoro)
  if(TARGET iocoro_warnings)
    target_link_libraries(${EXE_NAME} PRIVATE iocoro_warnings)
  endif()
endmacro()

make_example(hello_io_context)
make_example(tcp_echo)
make_example(switch_executor)

