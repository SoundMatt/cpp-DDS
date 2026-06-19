add_executable(cppdds_tests
    test_relay.cpp
    test_dds.cpp
    test_mock.cpp
)

target_link_libraries(cppdds_tests
    PRIVATE
        cppdds_lib
        Catch2::Catch2WithMain
)

target_include_directories(cppdds_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

include(CTest)
include(Catch)
catch_discover_tests(cppdds_tests)
