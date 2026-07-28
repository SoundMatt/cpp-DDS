# interop/CMakeLists.txt — only ever included when CPPDDS_INTEROP_TESTS=ON
# (see the option guard in the root CMakeLists.txt). A separate executable
# target from cppdds_tests, so these tests are never built, linked, or
# registered with CTest by the default configuration — the CMake/CTest
# equivalent of go-DDS's `go:build interop` tag. See README.md in this
# directory for prerequisites and how to run this suite locally.

add_executable(cppdds_interop_tests
    test_interop.cpp
)

target_link_libraries(cppdds_interop_tests
    PRIVATE
        cppdds_lib
        Catch2::Catch2WithMain
)

target_include_directories(cppdds_interop_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

include(CTest)
include(Catch)
# LABELS "interop" additionally lets a caller who already opted in with
# CPPDDS_INTEROP_TESTS=ON further separate this suite from anything else
# registered in that same build (`ctest -L interop` / `ctest -LE interop`),
# on top of the primary exclusion mechanism above (this target isn't even
# built when the option is off).
#
# SKIP_RETURN_CODE 4 maps Catch2's own "every executed test case was
# skipped" process exit code (see catch_test_case_registry_impl.cpp's
# skippedTestCount handling — SKIP() is used, not REQUIRE failure, when no
# live CycloneDDS peer answers) onto CTest's "Not Run" result instead of
# "Failed": a missing peer container is a setup problem, not a
# wire-compatibility regression.
catch_discover_tests(cppdds_interop_tests
    TEST_PREFIX "interop:"
    PROPERTIES LABELS "interop" SKIP_RETURN_CODE 4
)
