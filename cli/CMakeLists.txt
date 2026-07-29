# cpp_dds_cli — subcommand dispatch + version/capabilities/status/conform/
# convert implementation, as a small static library so
# tests/test_cpp_dds_cli.cpp can exercise it in-process (mirrors
# ddstool_cli's own precedent — see top-level CMakeLists.txt comment for
# why this is unconditional, ahead of the executable's own opt-out option
# below).
add_library(cpp_dds_cli STATIC cpp_dds_cli.cpp)

target_link_libraries(cpp_dds_cli PUBLIC cppdds_lib)

target_include_directories(cpp_dds_cli
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# cpp-dds — the executable itself.
option(CPPDDS_BUILD_CLI "Build RELAY convert CLI" ON)
if(CPPDDS_BUILD_CLI)
    add_executable(cpp-dds main.cpp)
    target_link_libraries(cpp-dds PRIVATE cpp_dds_cli)
    install(TARGETS cpp-dds DESTINATION bin)
endif()
