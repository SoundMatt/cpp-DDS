cmake_minimum_required(VERSION 3.21)

project(cppdds
    VERSION 0.1.0
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
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/FetchDeps.cmake)

# ── Library ───────────────────────────────────────────────────────────────────

add_library(cppdds_lib STATIC
    src/relay.cpp
    src/dds.cpp
    src/mock/participant.cpp
)

target_include_directories(cppdds_lib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(Threads REQUIRED)
target_link_libraries(cppdds_lib PUBLIC Threads::Threads)

# ── Tests ─────────────────────────────────────────────────────────────────────

option(CPPDDS_BUILD_TESTS "Build tests" ON)
if(CPPDDS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── CLI binary ────────────────────────────────────────────────────────────────

option(CPPDDS_BUILD_CLI "Build RELAY convert CLI" ON)
if(CPPDDS_BUILD_CLI)
    add_subdirectory(cli)
endif()

message(STATUS "cpp-DDS v${PROJECT_VERSION} — C++${CMAKE_CXX_STANDARD} / ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
