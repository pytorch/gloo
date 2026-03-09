# gloo/transport/tcp/peel/CMakeLists.txt

set(PEEL_SOURCES
    peel_protocol.cc
    peel_redis.cc
    peel_full_mesh.cc
    peel_transport.cc
    peel_broadcast.cc
    peel_context.cc
)

set(PEEL_HEADERS
    peel_protocol.h
    peel_redis.h
    peel_full_mesh.h
    peel_transport.h
    peel_broadcast.h
    peel_context.h
)

# Find hiredis
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED hiredis)

# Main library
add_library(gloo_peel ${PEEL_SOURCES})

target_include_directories(gloo_peel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${HIREDIS_INCLUDE_DIRS}
)

target_link_libraries(gloo_peel
    PUBLIC
        ${HIREDIS_LIBRARIES}
)

# =============================================================================
# Test executables
# =============================================================================

# Test: Full Mesh Handshake
add_executable(test_peel_full_mesh test_peel_full_mesh.cc)
target_link_libraries(test_peel_full_mesh gloo_peel)

# Test: Gloo Context
add_executable(test_peel_gloo_context test_peel_gloo_context.cc)
target_link_libraries(test_peel_gloo_context gloo_peel)

# Test: Broadcast Example
add_executable(test_peel_broadcast_example test_peel_broadcast_example.cc)
target_link_libraries(test_peel_broadcast_example gloo_peel)

# =============================================================================
# Install
# =============================================================================

install(TARGETS gloo_peel
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${PEEL_HEADERS}
    DESTINATION include/gloo/transport/tcp/peel
)

# Optionally install test binaries
install(TARGETS 
    test_peel_full_mesh 
    test_peel_gloo_context 
    test_peel_broadcast_example
    RUNTIME DESTINATION bin
    OPTIONAL
)