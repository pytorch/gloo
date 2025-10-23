find_package(OpenSSL 1.1.1 EXACT)

set(GLOO_TEST_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/allgather_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/allgatherv_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/allreduce_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/alltoall_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/alltoallv_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/barrier_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/base_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/broadcast_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/gather_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/gatherv_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/main.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/memory_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/reduce_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/remote_key_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/send_recv_test.cc"
  )
set(GLOO_TEST_LIBRARIES ${gloo_ALL_TARGETS_DEPENDENCY_LIBS})

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  list(APPEND GLOO_TEST_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/linux_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/multiproc_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/transport_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tcp_test.cc"
    )
  list(APPEND GLOO_TEST_LIBRARIES rt)
endif()

if(${OpenSSL_FOUND})
  list(APPEND GLOO_TEST_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/openssl_utils.cc"
  )
  list(APPEND GLOO_TEST_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
endif()


add_executable(gloo_test ${GLOO_TEST_SRCS})
target_link_libraries(gloo_test gloo gtest ${GLOO_TEST_LIBRARIES})

if(MSVC AND USE_LIBUV)
  add_custom_command(TARGET gloo_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${libuv_DLL_PATH}"
          $<TARGET_FILE_DIR:gloo_test>)
endif()

if(USE_CUDA)
  if(NOT MSVC)
    set(GLOO_TEST_CUDA_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/base_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/cuda_allreduce_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/cuda_base_test.cu"
    "${CMAKE_CURRENT_SOURCE_DIR}/cuda_broadcast_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/openssl_utils.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cc"
    )

    if(GLOO_USE_CUDA_TOOLKIT)
      enable_language(CUDA)
      add_executable(gloo_test_cuda ${GLOO_TEST_CUDA_SRCS})
    else()
      cuda_add_executable(gloo_test_cuda ${GLOO_TEST_CUDA_SRCS})
    endif()
    target_link_libraries(gloo_test_cuda gloo_cuda gtest ${GLOO_TEST_LIBRARIES} ${GLOO_CUDA_LIBRARIES})
  endif()
endif()

if(USE_ROCM)
  set(GLOO_TEST_HIP_SRCS
    "${HIPIFY_OUTPUT_DIR}/test/hip_allreduce_test.cc"
    "${HIPIFY_OUTPUT_DIR}/test/hip_base_test.hip"
    "${HIPIFY_OUTPUT_DIR}/test/hip_broadcast_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/base_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/openssl_utils.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cc"
    )

  gloo_hip_add_executable(gloo_test_hip ${GLOO_TEST_HIP_SRCS})
  target_link_libraries(gloo_test_hip gloo_hip gtest ${GLOO_TEST_LIBRARIES})
endif()
