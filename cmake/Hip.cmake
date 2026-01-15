set(HAVE_HIP FALSE)

# ROCm path configuration (still needed for finding hip package)
if(NOT DEFINED ENV{ROCM_PATH})
  set(ROCM_PATH /opt/rocm)
else()
  set(ROCM_PATH $ENV{ROCM_PATH})
endif()

# Architecture handling with the following priority:
# 1. GLOO_ROCM_ARCH (env or cache) takes priority
# 2. Fall back to CMAKE_HIP_ARCHITECTURES if set
# 3. Use defaults if neither is set
set(GLOO_ROCM_ARCH_DEFAULT "gfx906;gfx908;gfx90a")

if(DEFINED ENV{GLOO_ROCM_ARCH})
  set(GLOO_ROCM_ARCH $ENV{GLOO_ROCM_ARCH} CACHE STRING "HIP architectures for Gloo")
elseif(NOT DEFINED GLOO_ROCM_ARCH)
  if(DEFINED CMAKE_HIP_ARCHITECTURES)
    set(GLOO_ROCM_ARCH ${CMAKE_HIP_ARCHITECTURES} CACHE STRING "HIP architectures for Gloo")
  else()
    set(GLOO_ROCM_ARCH ${GLOO_ROCM_ARCH_DEFAULT} CACHE STRING "HIP architectures for Gloo")
  endif()
endif()

# Warn if CMAKE_HIP_ARCHITECTURES differs from GLOO_ROCM_ARCH
if(DEFINED CMAKE_HIP_ARCHITECTURES AND NOT "${CMAKE_HIP_ARCHITECTURES}" STREQUAL "${GLOO_ROCM_ARCH}")
  message(WARNING "CMAKE_HIP_ARCHITECTURES (${CMAKE_HIP_ARCHITECTURES}) differs from GLOO_ROCM_ARCH (${GLOO_ROCM_ARCH}). Using GLOO_ROCM_ARCH for Gloo targets.")
endif()

# Enable HIP language
enable_language(HIP)

# Find HIP package for hip::host target
find_package(hip REQUIRED)

if(hip_FOUND)
  set(HAVE_HIP TRUE)
  message(STATUS "Found HIP: ${hip_VERSION}")
  message(STATUS "GLOO_ROCM_ARCH: ${GLOO_ROCM_ARCH}")
endif()

################################################################################
# Helper function for HIP libraries using CMake native support
function(gloo_hip_add_library target)
  set(sources ${ARGN})

  # Mark non-.hip files as HIP language
  foreach(source ${sources})
    get_filename_component(ext ${source} LAST_EXT)
    if(NOT "${ext}" STREQUAL ".hip")
      set_source_files_properties(${source} PROPERTIES LANGUAGE HIP)
    endif()
  endforeach()

  add_library(${target} ${GLOO_STATIC_OR_SHARED} ${sources})
  target_include_directories(${target} PUBLIC ${GLOO_HIP_INCLUDE})
  target_link_libraries(${target} PRIVATE hip::host ${gloo_hip_DEPENDENCY_LIBS})
  target_compile_options(${target} PRIVATE ${GLOO_HIP_FLAGS})

  # Set target-specific properties
  set_target_properties(${target} PROPERTIES
    HIP_ARCHITECTURES "${GLOO_ROCM_ARCH}"
    POSITION_INDEPENDENT_CODE ON
  )
endfunction()

# Helper function for HIP executables using CMake native support
function(gloo_hip_add_executable target)
  set(sources ${ARGN})

  # Mark non-.hip files as HIP language
  foreach(source ${sources})
    get_filename_component(ext ${source} LAST_EXT)
    if(NOT "${ext}" STREQUAL ".hip")
      set_source_files_properties(${source} PROPERTIES LANGUAGE HIP)
    endif()
  endforeach()

  add_executable(${target} ${sources})
  target_include_directories(${target} PUBLIC ${GLOO_HIP_INCLUDE})
  target_link_libraries(${target} PRIVATE hip::host ${gloo_hip_DEPENDENCY_LIBS})
  target_compile_options(${target} PRIVATE ${GLOO_HIP_FLAGS})

  # Set target-specific properties
  set_target_properties(${target} PROPERTIES
    HIP_ARCHITECTURES "${GLOO_ROCM_ARCH}"
    POSITION_INDEPENDENT_CODE ON
  )
endfunction()
