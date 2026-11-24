function (prepend OUTPUT PREPEND)
  set(OUT "")
  foreach(ITEM ${ARGN})
    list(APPEND OUT "${PREPEND}${ITEM}")
  endforeach()
  set(${OUTPUT} ${OUT} PARENT_SCOPE)
endfunction(prepend)

set(HIPIFY_SCRIPTS_DIR ${PROJECT_SOURCE_DIR}/tools/amd_build)
file(GLOB HIPIFY_SCRIPTS ${HIPIFY_SCRIPTS_DIR}/*.py)
set(HIPIFY_OUTPUT_ROOT_DIR ${PROJECT_BINARY_DIR}/hip)

find_package (Python3 COMPONENTS Interpreter)
set(HIPIFY_COMMAND
  ${Python3_EXECUTABLE} ${HIPIFY_SCRIPTS_DIR}/build_amd.py
  --project-directory ${PROJECT_SOURCE_DIR}
  --output-directory ${HIPIFY_OUTPUT_ROOT_DIR}
)
execute_process(
  COMMAND python ${HIPIFY_COMMAND} --list-files-only
  OUTPUT_VARIABLE HIPIFY_FILES
  RESULT_VARIABLE hipify_return_value
)
if (NOT hipify_return_value EQUAL 0)
  message("hipify_return_value ${hipify_return_value}")
  message(FATAL_ERROR "Failed to get hipify files list!")
endif()

prepend(HIPIFY_INPUT_FILES "${PROJECT_SOURCE_DIR}/" "${HIPIFY_FILES}")
prepend(HIPIFY_OUTPUT_FILES "${HIPIFY_OUTPUT_ROOT_DIR}/" "${HIPIFY_FILES}")
# add_custom_command(
#   OUTPUT ${HIPIFY_OUTPUT_FILES}
#   DEPENDS ${HIPIFY_INPUT_FILES} ${HIPIFY_SCRIPTS}
#   COMMAND ${HIPIFY_COMMAND})

execute_process(
  COMMAND python ${HIPIFY_COMMAND}
  RESULT_VARIABLE hipify_return_value
  )
if (NOT hipify_return_value EQUAL 0)
  message("hipify_return_value ${hipify_return_value}")
  message(FATAL_ERROR "Failed to get hipify files list!")
endif()

include_directories(PREPEND ${HIPIFY_OUTPUT_ROOT_DIR})
set(HIPIFY_OUTPUT_DIR ${HIPIFY_OUTPUT_ROOT_DIR}/gloo)
