set(GLOO_BENCHMARK_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/main.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/options.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/runner.cc"
  )

add_executable(benchmark ${GLOO_BENCHMARK_SRCS})
target_link_libraries(benchmark gloo ${gloo_ALL_TARGETS_DEPENDENCY_LIBS})

if(GLOO_INSTALL)
  install(TARGETS benchmark DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
endif()

if(USE_CUDA)
  set(GLOO_BENCHMARK_CUDA_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/cuda_main.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/options.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runner.cc"
    )

  add_executable(benchmark_cuda ${GLOO_BENCHMARK_CUDA_SRCS})
  target_link_libraries(benchmark_cuda gloo_cuda ${GLOO_CUDA_LIBRARIES})

  if(GLOO_INSTALL)
    install(TARGETS benchmark_cuda DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
  endif()
endif()

if(USE_ROCM)
  set(GLOO_BENCHMARK_HIP_SRCS
    "${HIPIFY_OUTPUT_DIR}/benchmark/hip_main.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/options.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runner.cc"
    )

  gloo_hip_add_executable(benchmark_hip ${GLOO_BENCHMARK_HIP_SRCS})
  target_link_libraries(benchmark_hip gloo_hip)

  if(GLOO_INSTALL)
    install(TARGETS benchmark_hip DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
  endif()
endif()
