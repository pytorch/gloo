set(HAVE_HIP FALSE)

IF(NOT DEFINED ENV{ROCM_PATH})
  SET(ROCM_PATH /opt/rocm)
ELSE()
  SET(ROCM_PATH $ENV{ROCM_PATH})
ENDIF()

IF(NOT DEFINED ENV{GLOO_ROCM_ARCH})
  SET(GLOO_ROCM_ARCH gfx906;gfx908;gfx90a)
ELSE()
  SET(GLOO_ROCM_ARCH $ENV{GLOO_ROCM_ARCH})
ENDIF()

# Add HIP to the CMAKE Module Path
set(CMAKE_MODULE_PATH ${ROCM_PATH}/lib/cmake/hip ${CMAKE_MODULE_PATH})

# Disable Asserts In Code (Can't use asserts on HIP stack.)
ADD_DEFINITIONS(-DNDEBUG)

# Find the HIP Package
enable_language(HIP)
find_package(HIP)
# Require AMD HIP platform
add_definitions(-D__HIP_PLATFORM_AMD__ -D__HIP_PLATFORM_HCC__)

IF(HIP_FOUND)
  set(HAVE_HIP TRUE)

  set(hip_library_name amdhip64)
  message("HIP library name: ${hip_library_name}")

  set(CMAKE_HCC_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG})
  set(CMAKE_HCC_FLAGS_RELEASE ${CMAKE_CXX_FLAGS_RELEASE})
  FIND_LIBRARY(GLOO_HIP_HCC_LIBRARIES ${hip_library_name} HINTS ${ROCM_PATH}/lib)

ENDIF()

################################################################################
function(gloo_hip_add_library target)
  set(sources ${ARGN})
  set_source_files_properties(${sources} PROPERTIES HIP_SOURCE_PROPERTY_FORMAT 1)
  add_library(${target} ${GLOO_STATIC_OR_SHARED} ${sources})
  target_include_directories(${target} PUBLIC ${GLOO_HIP_INCLUDE})
  target_compile_options(${target} PUBLIC ${HIP_CXX_FLAGS})
  target_link_libraries(${target} ${gloo_hip_DEPENDENCY_LIBS})
endfunction()

function(gloo_hip_add_executable target)
  set(sources ${ARGN})
  set_source_files_properties(${sources} PROPERTIES HIP_SOURCE_PROPERTY_FORMAT 1)
  add_executable(${target} ${sources})
  target_include_directories(${target} PUBLIC ${GLOO_HIP_INCLUDE})
  target_compile_options(${target} PUBLIC ${HIP_CXX_FLAGS})
  target_link_libraries(${target} ${gloo_hip_DEPENDENCY_LIBS})
endfunction()
