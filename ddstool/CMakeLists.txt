# ddstool_cli — subcommand dispatch + `idl` subcommand implementation, as a
# small static library so tests/test_ddstool_cli.cpp can exercise it
# in-process (see top-level CMakeLists.txt comment for why this is
# unconditional, ahead of the executable's own opt-out option below).
add_library(ddstool_cli STATIC cli.cpp)

target_link_libraries(ddstool_cli PUBLIC cppdds_lib)

target_include_directories(ddstool_cli
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# ddstool — the executable itself (distinct from the existing `cpp-dds`
# conformance CLI under cli/).
option(CPPDDS_BUILD_DDSTOOL "Build the ddstool IDL-compiler CLI executable" ON)
if(CPPDDS_BUILD_DDSTOOL)
    add_executable(ddstool main.cpp)
    target_link_libraries(ddstool PRIVATE ddstool_cli)
    install(TARGETS ddstool DESTINATION bin)
endif()
