cmake_minimum_required(VERSION 3.21)

project(cppdds
    VERSION 0.23.0
    DESCRIPTION "C++ DDS library — RELAY-conformant"
    HOMEPAGE_URL "https://github.com/SoundMatt/cpp-DDS"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual
        -Wno-unused-parameter
    )
endif()
if(MSVC)
    add_compile_options(
        /W4
        /wd4100   # unreferenced formal parameter
        /wd4996   # deprecated CRT functions
        /utf-8    # treat sources and output as UTF-8
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/FetchDeps.cmake)

# ── Library ───────────────────────────────────────────────────────────────────

add_library(cppdds_lib STATIC
    src/relay.cpp
    src/dds.cpp
    src/mock/participant.cpp
    src/rtps/types.cpp
    src/rtps/cdr.cpp
    src/rtps/transport.cpp
    src/rtps/spdp.cpp
    src/rtps/sedp.cpp
    src/rtps/participant.cpp
    src/rtps/persist.cpp
    src/safety/e2e.cpp
    src/security/crypto/sha256.cpp
    src/security/crypto/aes.cpp
    src/security/crypto/gcm.cpp
    src/security/security.cpp
    src/security/access.cpp
    src/security/replay.cpp
    src/xtypes/detail/sha256.cpp
    src/xtypes/xtypes.cpp
    src/cdr/cdr.cpp
    src/idl/parser.cpp
    src/idl/gen.cpp
    src/tsn/tsn.cpp
    src/tsn/taprio.cpp
)

# Platform-specific socket tuning (SO_PRIORITY / IP_TOS / SO_TXTIME /
# CLOCK_TAI): mirrors go-DDS's rtps/traffic_linux.go vs. rtps/traffic_other.go
# `//go:build` split as a translation-unit split rather than an in-file
# #ifdef (see ROADMAP.md, Tier 1 phase 3, and include/dds/rtps/traffic.hpp).
#
# Same split for dds::tsn's TAPRIO netlink integration (ROADMAP.md's Tier 3
# `tsn` item): src/tsn/taprio_linux.cpp (real RTM_NEWQDISC/RTM_GETQDISC via
# NETLINK_ROUTE) vs. src/tsn/taprio_other.cpp (portable "not supported"
# stub) — see include/dds/tsn/taprio.hpp.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(cppdds_lib PRIVATE src/rtps/traffic_linux.cpp src/tsn/taprio_linux.cpp)
else()
    target_sources(cppdds_lib PRIVATE src/rtps/traffic_other.cpp src/tsn/taprio_other.cpp)
endif()

target_include_directories(cppdds_lib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(Threads REQUIRED)
target_link_libraries(cppdds_lib PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(cppdds_lib PUBLIC ws2_32)
endif()

# ── ddstool CLI library (IDL compiler; part of the proposed `cppdds_tools`
#    target group per ROADMAP.md's "Target architecture — 5-library split").
#    Unconditional and ahead of tests/ below: `ddstool_cli` is a small
#    static library (cli.cpp) that both the `ddstool` executable and
#    tests/test_ddstool_cli.cpp link against, so tests can exercise the
#    real subcommand-dispatch/exit-code behavior in-process rather than
#    only via a subprocess spawn. The `ddstool` *executable* target itself
#    remains separately opt-out via CPPDDS_BUILD_DDSTOOL — see
#    ddstool/CMakeLists.txt.
add_subdirectory(ddstool)

# ── cppdds_bridges (Tier 4 — bridges; ROADMAP.md's proposed `ddsbridges`
#    target group, first materialized here since Tier 4 is the first tier
#    with any bridge/ code to put in it — see ROADMAP.md's "Target
#    architecture — 5-library split" table). A separate static library,
#    opt-in like CPPDDS_BUILD_TESTS/CPPDDS_BUILD_CLI, depending on
#    cppdds_lib (kept as-is rather than renamed to cppdds_core: that rename
#    is unrelated scope this PR does not take on).
option(CPPDDS_BUILD_BRIDGES "Build the cppdds_bridges library (dds::bridge::*)" ON)
if(CPPDDS_BUILD_BRIDGES)
    add_library(cppdds_bridges STATIC
        src/bridge/grpc/grpc.cpp
        src/bridge/grpc/transport.cpp
        src/bridge/grpc/config.cpp
    )
    target_include_directories(cppdds_bridges
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(cppdds_bridges PUBLIC cppdds_lib)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

option(CPPDDS_BUILD_TESTS "Build tests" ON)
if(CPPDDS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Interop tests (opt-in, RTPS wire-level against a live CycloneDDS peer) ────
#
# Mirrors go-DDS's `go:build interop` tag: a dedicated CMake option, default
# OFF, producing a separate cppdds_interop_tests executable/CTest suite that
# is never built, linked, or registered by the default `cmake --build` /
# `ctest` invocation (including the CI build-and-test matrix). See
# interop/README.md for prerequisites and how to run this suite locally, and
# ROADMAP.md's "Interop testing infrastructure" section for why this exists
# alongside (not instead of) the byte-vector golden-vector tests in tests/.
option(CPPDDS_INTEROP_TESTS
    "Build RTPS wire-level interop tests against a live CycloneDDS peer (see interop/README.md)"
    OFF)
if(CPPDDS_INTEROP_TESTS)
    enable_testing()
    add_subdirectory(interop)
endif()

# ── CLI binary ────────────────────────────────────────────────────────────────

option(CPPDDS_BUILD_CLI "Build RELAY convert CLI" ON)
if(CPPDDS_BUILD_CLI)
    add_subdirectory(cli)
endif()

message(STATUS "cpp-DDS v${PROJECT_VERSION} — C++${CMAKE_CXX_STANDARD} / ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
