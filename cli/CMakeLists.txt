# cppdds_cli — subcommand dispatch + version/capabilities/status/conform/
# convert implementations, as a small static library so
# tests/test_cli_main.cpp can exercise the cpp-dds CLI in-process (see
# top-level CMakeLists.txt comment for why this is unconditional, ahead of
# the executable's own opt-out option below), mirroring ddstool's identical
# ddstool_cli split (see ddstool/CMakeLists.txt). Source file is named
# cpp_dds_cli.cpp/.hpp rather than cli.cpp/.hpp because ddstool/cli.hpp is a
# distinct header of the same bare name and both directories end up on
# tests/test_cli_main.cpp's include path (see cpp_dds_cli.hpp's doc
# comment).
add_library(cppdds_cli STATIC cpp_dds_cli.cpp)

target_link_libraries(cppdds_cli PUBLIC cppdds_lib)

target_include_directories(cppdds_cli
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# cpp-dds — the executable itself.
option(CPPDDS_BUILD_CLI "Build RELAY convert CLI" ON)
if(CPPDDS_BUILD_CLI)
    add_executable(cpp-dds main.cpp)
    target_link_libraries(cpp-dds PRIVATE cppdds_cli)
    install(TARGETS cpp-dds DESTINATION bin)
endif()
