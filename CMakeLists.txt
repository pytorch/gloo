cmake_minimum_required(VERSION 3.18 FATAL_ERROR)

project(gloo CXX C)

set(GLOO_VERSION_MAJOR 0)
set(GLOO_VERSION_MINOR 5)
set(GLOO_VERSION_PATCH 0)
set(GLOO_VERSION
    "${GLOO_VERSION_MAJOR}.${GLOO_VERSION_MINOR}.${GLOO_VERSION_PATCH}")

# Gloo assumes 64-bit and doesn't run builds/tests for anything else.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Gloo can only be built on 64-bit systems.")
endif()

# Local CMake modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)

# Build target options
option(BUILD_TEST "Build test binary (requires gtest)" OFF)
option(BUILD_BENCHMARK "Build benchmark binary (requires hiredis)" OFF)

# Option defaults (so they can be overwritten before declaring the option)
set(USE_REDIS_DEFAULT OFF)
set(USE_IBVERBS_DEFAULT OFF)
set(USE_NCCL_DEFAULT OFF)
set(USE_RCCL_DEFAULT OFF)
set(USE_LIBUV_DEFAULT OFF)
set(USE_TCP_OPENSSL_LINK_DEFAULT OFF)
set(USE_TCP_OPENSSL_LOAD_DEFAULT OFF)

# Options
option(USE_REDIS "Support using Redis for rendezvous" ${USE_REDIS_DEFAULT})
option(USE_IBVERBS "Support ibverbs transport" ${USE_IBVERBS_DEFAULT})
option(USE_NCCL "Support using NCCL for local collectives" ${USE_NCCL_DEFAULT})
option(USE_RCCL "Support using RCCL for local collectives" ${USE_RCCL_DEFAULT})
option(USE_LIBUV "Build libuv transport" ${USE_LIBUV_DEFAULT})
option(USE_TCP_OPENSSL_LINK "Build TCP-TLS transport with dynamically linked OpenSSL (Linux only)" ${USE_TCP_OPENSSL_LINK_DEFAULT})
option(USE_TCP_OPENSSL_LOAD "Build TCP-TLS transport with OpenSSL dynamically loaded during runtime (Linux only)" ${USE_TCP_OPENSSL_LOAD_DEFAULT})
if(${USE_TCP_OPENSSL_LINK} AND ${USE_TCP_OPENSSL_LOAD})
  message(FATAL_ERROR "USE_TCP_OPENSSL_LINK and USE_TCP_OPENSSL_LOAD are mutually exclusive")
endif()
option(USE_CUDA "Build with CUDA support" OFF)
option(GLOO_USE_CUDA_TOOLKIT "Build CUDA with FindCUDATookit.cmake and enable_language(CUDA)" OFF)
option(GLOO_USE_TORCH_DTYPES "Build CUDA kernels with pytorch dtypes" 0)

if(MSVC)
  message(STATUS "MSVC detected")
  set(USE_REDIS OFF)
  message(STATUS "Set USE_REDIS OFF")
  set(USE_IBVERBS OFF)
  message(STATUS "Set USE_IBVERBS OFF")
  set(USE_NCCL OFF)
  message(STATUS "Set USE_NCCL OFF")
  set(USE_RCCL OFF)
  message(STATUS "Set USE_RCCL OFF")
  set(USE_LIBUV OFF)
  message(STATUS "Set USE_LIBUV OFF")
  # message(STATUS "Only USE_LIBUV is supported on Windows")

  if(BUILD_BENCHMARK)
    message(FATAL_ERROR "BUILD_BENCHMARK is not supported on Windows yet")
  endif()
endif()

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "Build type not set -- defaulting to Release")
  set(CMAKE_BUILD_TYPE "Release")
endif()

# Use CMAKE_<LANG>_COMPILER_LAUNCHER if available.
find_program(SCCACHE_EXECUTABLE sccache)
mark_as_advanced(SCCACHE_EXECUTABLE)
if(SCCACHE_EXECUTABLE)
  if(MSVC)
    set(SCCACHE_COMPILE_MATCH_STRING ".*/sccache-cl.exe$")
  else()
    set(SCCACHE_COMPILE_MATCH_STRING ".*/s?ccache$")
  endif()

  foreach(LANG CXX C)
    if(NOT DEFINED CMAKE_${LANG}_COMPILER_LAUNCHER AND NOT CMAKE_${LANG}_COMPILER MATCHES ${SCCACHE_COMPILE_MATCH_STRING})
      message(STATUS "Enabling sccache for ${LANG}")
      set(CMAKE_${LANG}_COMPILER_LAUNCHER ${SCCACHE_EXECUTABLE} CACHE STRING "")
    endif()
  endforeach()
endif()

# Define sanitizer option, if specified.
if(SANITIZE)
  add_definitions("-fsanitize=${SANITIZE}")
  add_definitions("-fno-omit-frame-pointer")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=${SANITIZE}")
endif()

# Add install targets by default (override from parent project)
set(GLOO_INSTALL ON CACHE BOOL "")
mark_as_advanced(GLOO_INSTALL)

# Compile definitions for Gloo
set(GLOO_COMPILE_DEFS)

# Build shared or static libraries (override from parent project)
if(BUILD_SHARED_LIBS)
  set(GLOO_STATIC_OR_SHARED SHARED CACHE STRING "")
  message(STATUS "Gloo build as SHARED library")
else()
  set(GLOO_STATIC_OR_SHARED STATIC CACHE STRING "")
  message(STATUS "Gloo build as STATIC library")
endif()
mark_as_advanced(GLOO_STATIC_OR_SHARED)

# Process dependencies
include(cmake/Dependencies.cmake)

# Use project root as default include directory
include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_BINARY_DIR})

# Compiler flags

if(NOT MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -fPIC")
endif()

# Recurse into main project directory
add_subdirectory(gloo)

# Finally, create the cmake configuration files.
configure_file(
  ${PROJECT_SOURCE_DIR}/cmake/GlooConfigVersion.cmake.in
  ${PROJECT_BINARY_DIR}/cmake/GlooConfigVersion.cmake
  @ONLY)
configure_file(
  ${PROJECT_SOURCE_DIR}/cmake/GlooConfig.cmake.in
  ${PROJECT_BINARY_DIR}/cmake/GlooConfig.cmake
  @ONLY)

if(GLOO_INSTALL)
  if(NOT MSVC)
    install(FILES
      ${PROJECT_BINARY_DIR}/cmake/GlooConfig.cmake
      ${PROJECT_BINARY_DIR}/cmake/GlooConfigVersion.cmake
      DESTINATION share/cmake/Gloo
      COMPONENT dev)
    install(EXPORT GlooTargets DESTINATION share/cmake/Gloo
      FILE GlooTargets.cmake
      COMPONENT dev)
  else()
    install(FILES ${libuv_DLL_PATH} DESTINATION lib)
  endif()
endif()
