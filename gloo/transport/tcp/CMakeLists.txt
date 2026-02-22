if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    message(WARNING "Gloo TCP transport is supported only on Linux(uses epoll API)")
else()
    list(APPEND GLOO_TRANSPORT_SRCS
      "${CMAKE_CURRENT_SOURCE_DIR}/address.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/buffer.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/context.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/debug_logger.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/device.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/error.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/helpers.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/listener.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/loop.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/pair.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/socket.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/unbound_buffer.cc"
            # Peel protocol support
      "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_protocol.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_handshake.cc"
      )
    list(APPEND GLOO_TRANSPORT_HDRS
      "${CMAKE_CURRENT_SOURCE_DIR}/address.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/attr.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/buffer.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/context.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/debug_data.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/debug_logger.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/device.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/error.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/helpers.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/listener.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/loop.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/pair.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/socket.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/unbound_buffer.h"
            # Peel protocol headers
      "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_handshake.h"
      )
endif()

if(GLOO_HAVE_TRANSPORT_TCP_TLS)
    add_subdirectory(tls)
endif()

# Test executable for Peel + Gloo Context integration
if(BUILD_TEST)
    add_executable(test_peel_gloo_context
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/test_peel_gloo_context.cc"
    )
    target_link_libraries(test_peel_gloo_context gloo)
    target_include_directories(test_peel_gloo_context PRIVATE 
        ${PROJECT_SOURCE_DIR}
    )
endif()

set(GLOO_TRANSPORT_SRCS ${GLOO_TRANSPORT_SRCS} PARENT_SCOPE)
set(GLOO_TRANSPORT_HDRS ${GLOO_TRANSPORT_HDRS} PARENT_SCOPE)
