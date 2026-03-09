# gloo/transport/tcp/CMakeLists.txt

if(NOT ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    message(WARNING "Gloo TCP transport is supported only on Linux (uses epoll API)")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_redis.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_full_mesh.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_transport.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_broadcast.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_context.cc"
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
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_redis.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_full_mesh.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_transport.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_broadcast.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/peel/peel_context.h"
    )
endif()

if(GLOO_HAVE_TRANSPORT_TCP_TLS)
    add_subdirectory(tls)
endif()

# Add peel subdirectory for test executables
add_subdirectory(peel)

set(GLOO_TRANSPORT_SRCS ${GLOO_TRANSPORT_SRCS} PARENT_SCOPE)
set(GLOO_TRANSPORT_HDRS ${GLOO_TRANSPORT_HDRS} PARENT_SCOPE)