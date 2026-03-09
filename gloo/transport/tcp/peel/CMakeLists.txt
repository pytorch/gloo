# gloo/transport/tcp/peel/CMakeLists.txt

# Find hiredis (required for peel)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED hiredis)

# Add hiredis to parent scope for linking with gloo
set(HIREDIS_INCLUDE_DIRS ${HIREDIS_INCLUDE_DIRS} PARENT_SCOPE)
set(HIREDIS_LIBRARIES ${HIREDIS_LIBRARIES} PARENT_SCOPE)

# =============================================================================
# Test executables (standalone, link against gloo which includes peel sources)
# =============================================================================

# Test: Full Mesh Handshake
add_executable(test_peel_full_mesh test_peel_full_mesh.cc)
target_link_libraries(test_peel_full_mesh gloo ${HIREDIS_LIBRARIES})
target_include_directories(test_peel_full_mesh PRIVATE 
    ${PROJECT_SOURCE_DIR}
    ${HIREDIS_INCLUDE_DIRS}
)

# Test: Gloo Context
add_executable(test_peel_gloo_context test_peel_gloo_context.cc)
target_link_libraries(test_peel_gloo_context gloo ${HIREDIS_LIBRARIES})
target_include_directories(test_peel_gloo_context PRIVATE 
    ${PROJECT_SOURCE_DIR}
    ${HIREDIS_INCLUDE_DIRS}
)

# Test: Broadcast Example
add_executable(test_peel_broadcast_example test_peel_broadcast_example.cc)
target_link_libraries(test_peel_broadcast_example gloo ${HIREDIS_LIBRARIES})
target_include_directories(test_peel_broadcast_example PRIVATE 
    ${PROJECT_SOURCE_DIR}
    ${HIREDIS_INCLUDE_DIRS}
)