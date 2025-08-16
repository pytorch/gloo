set(gloo_DEPENDENCY_LIBS "")
set(gloo_cuda_DEPENDENCY_LIBS "")
set(gloo_hip_DEPENDENCY_LIBS "")

# Configure path to modules (for find_package)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${PROJECT_SOURCE_DIR}/cmake/Modules/")

if(USE_REDIS)
  find_package(hiredis REQUIRED)
  if(HIREDIS_FOUND)
    include_directories(SYSTEM ${HIREDIS_INCLUDE_DIRS})
    list(APPEND gloo_DEPENDENCY_LIBS ${HIREDIS_LIBRARIES})
  else()
    message(WARNING "Not compiling with Redis support. Suppress this warning with -DUSE_REDIS=OFF.")
    set(USE_REDIS OFF)
  endif()
endif()

if(USE_IBVERBS)
  find_package(ibverbs REQUIRED)
  if(IBVERBS_FOUND)
    include_directories(SYSTEM ${IBVERBS_INCLUDE_DIRS})
    list(APPEND gloo_DEPENDENCY_LIBS ${IBVERBS_LIBRARIES})
  else()
    message(WARNING "Not compiling with ibverbs support. Suppress this warning with -DUSE_IBVERBS=OFF.")
    set(USE_IBVERBS OFF)
  endif()
endif()

if(USE_LIBUV)
  # If the Gloo build is included from another project's build, it may
  # have already included libuv and we can use it directly here.
  if(TARGET uv_a)
    # Note: the CMake files in the libuv don't specify an include
    # directory for the uv and uv_a targets. If you're including the
    # Gloo build from your own project's build, and include libuv
    # there as well, you may need to include the following to tack on
    # the include path to the libuv targets.
    #
    #   set_target_properties(uv_a PROPERTIES
    #     INTERFACE_INCLUDE_DIRECTORIES "${libuv_SOURCE_DIR}/include"
    #     )
    #
  else()
    if(MSVC)
      find_library(
        libuv_LIBRARY
        NAMES uv libuv
        HINTS ${libuv_ROOT} ENV libuv_ROOT
        PATH_SUFFIXES lib/release lib/debug lib
        REQUIRED
        NO_DEFAULT_PATH)
      if(NOT EXISTS ${libuv_LIBRARY})
        message(FATAL_ERROR "Unable to find static libuv library in " $ENV{libuv_ROOT})
      endif()

      find_file(
        libuv_DLL_PATH
        NAMES uv.dll
        HINTS ${libuv_ROOT} ENV libuv_ROOT
        PATH_SUFFIXES lib/release lib/debug bin
        REQUIRED
        NO_DEFAULT_PATH)
      if(NOT EXISTS ${libuv_DLL_PATH})
        message(FATAL_ERROR "Unable to find uv.dll in " $ENV{libuv_ROOT})
      endif()

      find_file(
        uv_HEADER_PATH
        NAMES uv.h
        HINTS ${libuv_ROOT} ENV libuv_ROOT
        PATH_SUFFIXES include
        REQUIRED
        NO_DEFAULT_PATH)
      if(NOT EXISTS ${uv_HEADER_PATH})
        message(FATAL_ERROR "Unable to find headers of libuv in " $ENV{libuv_ROOT})
      endif()
      set(libuv_INCLUDE_DIRS ${uv_HEADER_PATH}/..)
    else()
      include(FindPkgConfig)
      pkg_search_module(libuv REQUIRED libuv>=1.26)
      find_file(
        libuv_LIBRARY
        NAMES libuv.a libuv_a.a
        PATHS ${libuv_LIBDIR}
        NO_DEFAULT_PATH)
      if(NOT EXISTS ${libuv_LIBRARY})
        message(FATAL_ERROR "Unable to find static libuv library in " ${libuv_LIBDIR})
      endif()
    endif()

    add_library(uv_a INTERFACE IMPORTED)
    set_target_properties(uv_a PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES ${libuv_INCLUDE_DIRS}
      INTERFACE_LINK_LIBRARIES ${libuv_LIBRARY}
      )
  endif()
endif()

if(USE_MPI)
  find_package(MPI)
  if(MPI_C_FOUND)
    message(STATUS "MPI include path: " ${MPI_CXX_INCLUDE_PATH})
    message(STATUS "MPI libraries: " ${MPI_CXX_LIBRARIES})
    include_directories(SYSTEM ${MPI_CXX_INCLUDE_PATH})
    list(APPEND gloo_DEPENDENCY_LIBS ${MPI_CXX_LIBRARIES})
    add_definitions(-DGLOO_USE_MPI=1)
  else()
    message(WARNING "Not compiling with MPI support. Suppress this warning with -DUSE_MPI=OFF.")
    set(USE_MPI OFF)
  endif()
endif()

if(USE_CUDA)
  include(cmake/Cuda.cmake)
  if(NOT HAVE_CUDA)
    message(WARNING "Not compiling with CUDA support. Suppress this warning with -DUSE_CUDA=OFF.")
    set(USE_CUDA OFF)
  endif()
endif()

if(USE_CUDA AND USE_NCCL)
  # NCCL_EXTERNAL is set if using the Caffe2 bundled version of NCCL
  if(NCCL_EXTERNAL)
    include_directories(SYSTEM ${NCCL_INCLUDE_DIRS})
    list(APPEND gloo_cuda_DEPENDENCY_LIBS ${NCCL_LIBRARIES} dl rt)
  else()
    find_package(nccl REQUIRED)
    if(NCCL_FOUND)
      include_directories(SYSTEM ${NCCL_INCLUDE_DIRS})
      list(APPEND gloo_cuda_DEPENDENCY_LIBS ${NCCL_LIBRARIES} dl rt)
    else()
      message(WARNING "Not compiling with NCCL support. Suppress this warning with -DUSE_NCCL=OFF.")
      set(USE_NCCL OFF)
    endif()
  endif()
endif()

if(USE_ROCM)
  include(cmake/Hip.cmake)
  if(HAVE_HIP)
    include(cmake/Hipify.cmake)
    list(APPEND HIP_CXX_FLAGS -fPIC)
    list(APPEND HIP_CXX_FLAGS -D__HIP_PLATFORM_AMD__=1)
    list(APPEND HIP_CXX_FLAGS -DCUDA_HAS_FP16=1)
    list(APPEND HIP_CXX_FLAGS -D__HIP_NO_HALF_OPERATORS__=1)
    list(APPEND HIP_CXX_FLAGS -D__HIP_NO_HALF_CONVERSIONS__=1)
    list(APPEND HIP_CXX_FLAGS -DHIP_VERSION=${HIP_VERSION_MAJOR})
    list(APPEND HIP_CXX_FLAGS -Wno-shift-count-negative)
    list(APPEND HIP_CXX_FLAGS -Wno-shift-count-overflow)
    list(APPEND HIP_CXX_FLAGS -Wno-duplicate-decl-specifier)
    list(APPEND HIP_CXX_FLAGS -DUSE_MIOPEN)

    set(HIP_CLANG_FLAGS ${HIP_CXX_FLAGS})
    # Ask hcc to generate device code during compilation so we can use
    # host linker to link.
    list(APPEND HIP_CLANG_FLAGS -fno-gpu-rdc)
    list(APPEND HIP_CLANG_FLAGS -Wno-defaulted-function-deleted)
    foreach(gloo_rocm_arch ${GLOO_ROCM_ARCH})
      list(APPEND HIP_CLANG_FLAGS --offload-arch=${gloo_rocm_arch})
    endforeach()

    set(GLOO_HIP_INCLUDE ${hip_INCLUDE_DIRS} $<BUILD_INTERFACE:${HIPIFY_OUTPUT_ROOT_DIR}> $<INSTALL_INTERFACE:include> ${GLOO_HIP_INCLUDE})

    # This is needed for library added by hip_add_library (same for hip_add_executable)
    hip_include_directories(${GLOO_HIP_INCLUDE})

    set(gloo_hip_DEPENDENCY_LIBS ${GLOO_HIP_HCC_LIBRARIES})

  else()
    message(WARNING "Not compiling with HIP support. Suppress this warning with -DUSE_ROCM=OFF.")
    set(USE_ROCM OFF)
  endif()
endif()

if(USE_ROCM AND USE_RCCL)
  find_package(rccl)
  if(RCCL_FOUND)
    include_directories(SYSTEM ${RCCL_INCLUDE_DIRS})
    list(APPEND gloo_hip_DEPENDENCY_LIBS ${RCCL_LIBRARIES} dl rt)
  else()
    message(WARNING "Not compiling with RCCL support. Suppress this warning with -DUSE_RCCL=OFF.")
    set(USE_RCCL OFF)
  endif()
endif()
# Make sure we can find googletest if building the tests
if(BUILD_TEST)
  # If the gtest target is already defined, we assume upstream knows
  # what they are doing and the version is new enough.
  if(NOT TARGET gtest)
    find_package(GTest REQUIRED)
    if(NOT GTEST_FOUND)
      message(FATAL_ERROR "Could not find googletest; cannot compile tests")
    endif()
    add_library(gtest INTERFACE)
    target_include_directories(gtest INTERFACE ${GTEST_INCLUDE_DIRS})
    target_link_libraries(gtest INTERFACE ${GTEST_LIBRARIES})
  endif()
endif()
